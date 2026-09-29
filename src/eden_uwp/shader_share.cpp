// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/shader_share.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/fs/path_util.h"
#include "eden_uwp/diagnostic.h"
#include "video_core/renderer_opengl/gl_shader_cache.h"

namespace EdenXbox {
namespace {
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;

constexpr std::uint64_t MaxCacheBytes = 64ull << 20;

std::string ShareUrl() {
    const char* url = std::getenv("NXBOX_SHADER_SHARE_URL");
    return url != nullptr ? std::string{url} : std::string{};
}

std::filesystem::path LocalCache(std::uint64_t title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
           fmt::format("{:016x}", title_id) / "opengl.bin";
}

Uri CacheUri(const std::string& base, std::uint64_t title_id) {
    return Uri{winrt::to_hstring(fmt::format("{}/{:016x}/opengl.bin", base, title_id))};
}
} // namespace

void DownloadSharedShaderCache(std::uint64_t title_id) {
    const std::string base = ShareUrl();
    if (base.empty() || title_id == 0) {
        return;
    }
    try {
        HttpClient client;
        const auto response = client.GetAsync(CacheUri(base, title_id)).get();
        if (response.StatusCode() != HttpStatusCode::Ok) {
            Diagnostic(fmt::format("SHADER_SHARE download status={}",
                                   static_cast<int>(response.StatusCode())));
            return;
        }
        const IBuffer buffer = response.Content().ReadAsBufferAsync().get();
        if (buffer.Length() == 0 || buffer.Length() > MaxCacheBytes) {
            Diagnostic(fmt::format("SHADER_SHARE download rejected size={}", buffer.Length()));
            return;
        }
        const auto local = LocalCache(title_id);
        std::filesystem::create_directories(local.parent_path());
        const auto incoming = local.parent_path() / "opengl.shared.bin";
        {
            std::ofstream out(incoming, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
        }
        const long added = OpenGL::ShaderCache::MergeCacheFiles(local, incoming);
        std::error_code ec;
        std::filesystem::remove(incoming, ec);
        Diagnostic(fmt::format("SHADER_SHARE downloaded {} bytes, merged {} new pipelines",
                               buffer.Length(), added));
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SHADER_SHARE download failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("SHADER_SHARE download failed ") + error.what());
    }
}

void UploadSharedShaderCache(std::uint64_t title_id) {
    const std::string base = ShareUrl();
    if (base.empty() || title_id == 0) {
        return;
    }
    try {
        const auto local = LocalCache(title_id);
        std::error_code ec;
        const auto size = std::filesystem::file_size(local, ec);
        if (ec || size <= 12 || size > MaxCacheBytes) {
            return;
        }
        std::vector<std::uint8_t> bytes(size);
        {
            std::ifstream in(local, std::ios::binary);
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        }
        DataWriter writer;
        writer.WriteBytes(bytes);
        HttpClient client;
        const auto response =
            client.PutAsync(CacheUri(base, title_id), HttpBufferContent{writer.DetachBuffer()}).get();
        Diagnostic(fmt::format("SHADER_SHARE uploaded {} bytes status={}", size,
                               static_cast<int>(response.StatusCode())));
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SHADER_SHARE upload failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("SHADER_SHARE upload failed ") + error.what());
    }
}

} // namespace EdenXbox
