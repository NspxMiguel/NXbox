// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/game_download.h"

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <thread>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox {
namespace {
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;

constexpr int MaxAttempts = 20;
constexpr uint32_t ChunkSize = 4u << 20;
namespace FS = Common::FS;

// Thrown by Transfer when the caller asked to stop; the partial file stays on disk.
struct DownloadCancelled {};

bool IsCancelled(const std::atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load();
}

// HTTP operations also run off the UI thread, with a timeout per request/read.
template <typename Operation>
auto WaitForDownload(Operation operation) {
    auto result = AwaitBounded(operation, std::chrono::seconds(60));
    if (!result) {
        throw std::runtime_error("network operation timed out");
    }
    return *result;
}

std::optional<std::uint64_t> AvailableSpace(const std::filesystem::path& folder) {
    return FreeSpace(folder);
}

std::uint64_t DownloadedSize(const std::filesystem::path& file) {
    return FS::IsFile(file) ? FS::GetSize(file) : 0;
}

std::uint64_t RemoteSize(HttpClient& client, const Uri& uri) {
    HttpRequestMessage request{HttpMethod::Head(), uri};
    const auto response =
        WaitForDownload(client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead));
    response.EnsureSuccessStatusCode();
    const auto length = response.Content().Headers().ContentLength();
    if (!length) {
        throw std::runtime_error("server did not report Content-Length");
    }
    return length.Value();
}

// One connection's worth of work: appends from the current file size until the server ends the
// stream. Returns the number of bytes written; a dropped connection throws.
void Transfer(HttpClient& client, const Uri& uri, const std::filesystem::path& destination,
              std::uint64_t total,
              const std::function<void(std::uint64_t, std::uint64_t)>& progress,
              const std::atomic<bool>* cancel) {
    std::uint64_t have = DownloadedSize(destination);
    HttpRequestMessage request{HttpMethod::Get(), uri};
    if (have != 0) {
        request.Headers().TryAppendWithoutValidation(
            L"Range", winrt::to_hstring("bytes=" + std::to_string(have) + "-"));
    }
    const auto response =
        WaitForDownload(client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead));
    response.EnsureSuccessStatusCode();
    if (have != 0 && response.StatusCode() != HttpStatusCode::PartialContent) {
        // The server ignored the Range header and is resending from byte 0.
        const auto free = AvailableSpace(destination.parent_path());
        // The existing partial file is reclaimed when opening with Write.
        if (!free || *free < total - have) {
            throw std::runtime_error("not enough space to restart the download");
        }
        have = 0;
    }
    FS::IOFile out(destination, have ? FS::FileAccessMode::Append : FS::FileAccessMode::Write);
    if (!out.IsOpen()) {
        throw std::runtime_error("cannot open " + destination.string());
    }
    const auto stream = WaitForDownload(response.Content().ReadAsInputStreamAsync());
    Buffer buffer{ChunkSize};
    auto sample_at = std::chrono::steady_clock::now();
    std::uint64_t sample_bytes = have;
    if (progress) {
        progress(have, total);
    }
    while (have < total) {
        if (IsCancelled(cancel)) {
            // Persist what was written so far; the next call resumes from the file size.
            out.Commit();
            throw DownloadCancelled{};
        }
        const auto read = WaitForDownload(stream.ReadAsync(buffer, ChunkSize, InputStreamOptions::Partial));
        if (read.Length() == 0) {
            throw std::runtime_error("connection closed early");
        }
        if (read.Length() > total - have) {
            throw std::runtime_error("server sent more bytes than expected");
        }
        if (out.WriteSpan(std::span<const std::uint8_t>{read.data(), read.Length()}) != read.Length()) {
            throw std::runtime_error("write failed (disk full?)");
        }
        have += read.Length();
        if (progress) {
            progress(have, total);
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - sample_at).count();
        if (elapsed >= 5.0) {
            Diagnostic(fmt::format("DOWNLOAD {}/{} MiB {:.1f} MiB/s", have >> 20, total >> 20,
                                   static_cast<double>(have - sample_bytes) / (1 << 20) / elapsed));
            sample_at = now;
            sample_bytes = have;
        }
    }
    if (!out.Commit()) {
        throw std::runtime_error("flush failed (disk full?)");
    }
}
} // namespace

bool DownloadFile(const std::string& url, std::filesystem::path& destination,
                  const std::function<void(const std::filesystem::path&)>& remember_target,
                  const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                  const std::atomic<bool>* cancel) {
    HttpClient client;
    const Uri uri{winrt::to_hstring(url)};
    for (int attempt = 1; attempt <= MaxAttempts; ++attempt) {
        if (IsCancelled(cancel)) {
            Diagnostic("DOWNLOAD_CANCELLED");
            return false;
        }
        try {
            if (!FS::CreateDirs(destination.parent_path())) {
                throw std::runtime_error("cannot create the download folder");
            }
            const auto total = RemoteSize(client, uri);
            auto have = DownloadedSize(destination);
            if (have > total) {
                if (!FS::RemoveFile(destination)) {
                    throw std::runtime_error("cannot remove an oversized download");
                }
                have = 0;
            }
            auto free = AvailableSpace(destination.parent_path());
            if (have != total && free && *free < total - have) {
                bool selected = false;
                for (const auto& folder : ExternalGameFolders()) {
                    const auto candidate = folder / destination.filename();
                    if (candidate == destination) {
                        continue;
                    }
                    const auto candidate_size = DownloadedSize(candidate);
                    const auto candidate_free = FreeSpace(folder);
                    // A partial download on another drive is resumed there. Do not copy the
                    // internal partial file: restarting externally needs the full remaining size.
                    if (candidate_size <= total && candidate_free &&
                        *candidate_free >= total - candidate_size) {
                        remember_target(candidate);
                        destination = candidate;
                        have = candidate_size;
                        free = candidate_free;
                        selected = true;
                        break;
                    }
                }
                if (!selected) {
                    Diagnostic(fmt::format("DOWNLOAD_NO_SPACE need={} MiB free={} MiB",
                                           (total - have) >> 20, *free >> 20));
                    return false;
                }
            }
            if (have != total && !free) {
                throw std::runtime_error("cannot determine download free space");
            }
            Diagnostic(fmt::format("DOWNLOAD_TARGET {} free={} MiB",
                                   FS::PathToUTF8String(destination), free.value_or(0) >> 20));
            if (have == total) {
                Diagnostic(fmt::format("DOWNLOAD_COMPLETE {} MiB", total >> 20));
                if (progress) {
                    progress(total, total);
                }
                return true;
            }
            Diagnostic(fmt::format("DOWNLOAD_START attempt={} have={} MiB total={} MiB free={} MiB",
                                   attempt, have >> 20, total >> 20, *free >> 20));
            Transfer(client, uri, destination, total, progress, cancel);
            if (DownloadedSize(destination) == total) {
                Diagnostic(fmt::format("DOWNLOAD_COMPLETE {} MiB", total >> 20));
                return true;
            }
        } catch (const DownloadCancelled&) {
            Diagnostic("DOWNLOAD_CANCELLED");
            return false;
        } catch (const winrt::hresult_error& error) {
            Diagnostic("DOWNLOAD_RETRY " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("DOWNLOAD_RETRY ") + error.what());
        }
        for (int tick = 0; tick < 20 && !IsCancelled(cancel); ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    Diagnostic("DOWNLOAD_FAILED");
    return false;
}

} // namespace EdenXbox
