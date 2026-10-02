// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/http_vfs_file.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "core/file_sys/vfs/vfs.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"

namespace EdenXbox {
namespace {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Web::Http;

constexpr std::uint64_t BlockSize = 16ull << 20;
constexpr std::size_t CachedBlocks = 4;
constexpr int Attempts = 6;

class HttpVfsFile final : public FileSys::VfsFile {
public:
    HttpVfsFile(std::string url, std::string name, std::uint64_t size)
        : url_(std::move(url)), name_(std::move(name)), size_(size),
          uri_(winrt::to_hstring(url_)) {}

    std::string GetName() const override {
        return name_;
    }
    std::size_t GetSize() const override {
        return static_cast<std::size_t>(size_);
    }
    bool Resize(std::size_t) override {
        return false;
    }
    FileSys::VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= size_) {
            return 0;
        }
        length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
        std::size_t done = 0;
        const std::lock_guard lock(mutex_);
        while (done < length) {
            const std::uint64_t at = offset + done;
            const std::uint64_t index = at / BlockSize;
            const std::vector<u8>* block = Block(index);
            if (block == nullptr) {
                break; // The caller sees a short read and fails cleanly.
            }
            const std::uint64_t within = at - index * BlockSize;
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(length - done, block->size() - within));
            std::memcpy(data + done, block->data() + within, count);
            done += count;
        }
        return done;
    }

private:
    // The cached block with this index, fetched when missing; nullptr after repeated failures.
    const std::vector<u8>* Block(std::uint64_t index) const {
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->first == index) {
                cache_.splice(cache_.begin(), cache_, it); // most recent first
                return &cache_.front().second;
            }
        }
        const std::uint64_t first = index * BlockSize;
        const std::uint64_t last = std::min(first + BlockSize, size_) - 1;
        for (int attempt = 1; attempt <= Attempts; ++attempt) {
            auto bytes = Fetch(first, last);
            if (bytes) {
                if (cache_.size() >= CachedBlocks) {
                    cache_.pop_back();
                }
                cache_.emplace_front(index, std::move(*bytes));
                return &cache_.front().second;
            }
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
        }
        Diagnostic(fmt::format("HTTP_FILE_FAILED {} bytes={}-{}", name_, first, last));
        return nullptr;
    }

    std::optional<std::vector<u8>> Fetch(std::uint64_t first, std::uint64_t last) const {
        try {
            HttpRequestMessage request{HttpMethod::Get(), uri_};
            request.Headers().TryAppendWithoutValidation(
                L"Range", winrt::to_hstring(fmt::format("bytes={}-{}", first, last)));
            const auto response = AwaitBounded(
                client_.SendRequestAsync(request, HttpCompletionOption::ResponseContentRead),
                std::chrono::seconds(120));
            if (!response || response->StatusCode() != HttpStatusCode::PartialContent) {
                return std::nullopt;
            }
            const auto buffer =
                AwaitBounded(response->Content().ReadAsBufferAsync(), std::chrono::seconds(120));
            if (!buffer || buffer->Length() != last - first + 1) {
                return std::nullopt;
            }
            return std::vector<u8>(buffer->data(), buffer->data() + buffer->Length());
        } catch (const winrt::hresult_error& error) {
            Diagnostic("HTTP_FILE_RETRY " + winrt::to_string(error.message()));
            return std::nullopt;
        }
    }

    std::string url_;
    std::string name_;
    std::uint64_t size_;
    Uri uri_;
    mutable HttpClient client_;
    mutable std::mutex mutex_;
    mutable std::list<std::pair<std::uint64_t, std::vector<u8>>> cache_;
};

} // namespace

FileSys::VirtualFile OpenHttpFile(const std::string& url) {
    try {
        HttpClient client;
        const Uri uri{winrt::to_hstring(url)};
        // One byte with a range: proves the server honours ranges and reports the total size.
        HttpRequestMessage request{HttpMethod::Get(), uri};
        request.Headers().TryAppendWithoutValidation(L"Range", L"bytes=0-0");
        const auto response = AwaitBounded(
            client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead),
            std::chrono::seconds(60));
        if (!response || response->StatusCode() != HttpStatusCode::PartialContent) {
            Diagnostic("HTTP_FILE no range support for " + url);
            return nullptr;
        }
        const auto range = response->Content().Headers().ContentRange();
        if (!range || !range.Length()) {
            Diagnostic("HTTP_FILE no total size for " + url);
            return nullptr;
        }
        const std::uint64_t size = range.Length().Value();
        std::string name = url.substr(url.find_last_of('/') + 1);
        name = winrt::to_string(Uri::UnescapeComponent(winrt::to_hstring(name)));
        Diagnostic(fmt::format("HTTP_FILE {} {} MiB", name, size >> 20));
        return std::make_shared<HttpVfsFile>(url, std::move(name), size);
    } catch (const winrt::hresult_error& error) {
        Diagnostic("HTTP_FILE failed " + winrt::to_string(error.message()));
        return nullptr;
    }
}

} // namespace EdenXbox
