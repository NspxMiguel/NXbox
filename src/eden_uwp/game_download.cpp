// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/game_download.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <thread>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "eden_uwp/diagnostic.h"

namespace EdenXbox {
namespace {
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;

constexpr int MaxAttempts = 20;
constexpr uint32_t ChunkSize = 4u << 20;

std::uint64_t RemoteSize(HttpClient& client, const Uri& uri) {
    HttpRequestMessage request{HttpMethod::Head(), uri};
    const auto response =
        client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead).get();
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
              std::uint64_t total) {
    std::error_code ec;
    std::uint64_t have =
        std::filesystem::exists(destination, ec) ? std::filesystem::file_size(destination, ec) : 0;
    HttpRequestMessage request{HttpMethod::Get(), uri};
    if (have != 0) {
        request.Headers().TryAppendWithoutValidation(
            L"Range", winrt::to_hstring("bytes=" + std::to_string(have) + "-"));
    }
    const auto response =
        client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead).get();
    response.EnsureSuccessStatusCode();
    if (have != 0 && response.StatusCode() != HttpStatusCode::PartialContent) {
        // The server ignored the Range header and is resending from byte 0.
        have = 0;
    }
    std::ofstream out(destination, std::ios::binary | (have ? std::ios::app : std::ios::trunc));
    if (!out) {
        throw std::runtime_error("cannot open " + destination.string());
    }
    const auto stream = response.Content().ReadAsInputStreamAsync().get();
    Buffer buffer{ChunkSize};
    auto sample_at = std::chrono::steady_clock::now();
    std::uint64_t sample_bytes = have;
    while (have < total) {
        const auto read = stream.ReadAsync(buffer, ChunkSize, InputStreamOptions::Partial).get();
        if (read.Length() == 0) {
            throw std::runtime_error("connection closed early");
        }
        out.write(reinterpret_cast<const char*>(read.data()), read.Length());
        if (!out) {
            throw std::runtime_error("write failed (disk full?)");
        }
        have += read.Length();
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - sample_at).count();
        if (elapsed >= 5.0) {
            Diagnostic(fmt::format("DOWNLOAD {}/{} MiB {:.1f} MiB/s", have >> 20, total >> 20,
                                   static_cast<double>(have - sample_bytes) / (1 << 20) / elapsed));
            sample_at = now;
            sample_bytes = have;
        }
    }
}
} // namespace

bool DownloadFile(const std::string& url, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    HttpClient client;
    const Uri uri{winrt::to_hstring(url)};
    for (int attempt = 1; attempt <= MaxAttempts; ++attempt) {
        try {
            const auto total = RemoteSize(client, uri);
            std::error_code ec;
            const auto have = std::filesystem::exists(destination, ec)
                                  ? std::filesystem::file_size(destination, ec)
                                  : 0;
            if (have == total) {
                Diagnostic(fmt::format("DOWNLOAD_COMPLETE {} MiB", total >> 20));
                return true;
            }
            if (have > total) {
                std::filesystem::remove(destination, ec);
            }
            Diagnostic(fmt::format("DOWNLOAD_START attempt={} have={} MiB total={} MiB", attempt,
                                   have >> 20, total >> 20));
            Transfer(client, uri, destination, total);
            continue;
        } catch (const winrt::hresult_error& error) {
            Diagnostic("DOWNLOAD_RETRY " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("DOWNLOAD_RETRY ") + error.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    Diagnostic("DOWNLOAD_FAILED");
    return false;
}

} // namespace EdenXbox
