// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/updater.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.Certificates.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/scope_exit.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/update_version.h"

namespace EdenXbox::Ui {
namespace {
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Headers;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter;

constexpr wchar_t kReleases[] = L"https://api.github.com/repos/NspxMiguel/NXbox/releases/latest";
constexpr wchar_t kAssetPrefix[] = L"https://github.com/NspxMiguel/NXbox/releases/download/";

struct ScopedApartment {
    ScopedApartment() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    ~ScopedApartment() {
        winrt::uninit_apartment();
    }
};

// Blocking only on the MTA worker, with bounded waits and cancellation on library teardown.
template <typename Operation>
auto Await(const Operation& operation, const std::atomic<bool>& cancelled,
           std::chrono::seconds timeout = std::chrono::seconds(60)) {
    struct Completion {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
    };
    // WinRT permits only one completion handler. Repeated operation.wait_for calls would
    // replace that handler after a timeout. Shared ownership also survives cancellation.
    const auto completion = std::make_shared<Completion>();
    operation.Completed([completion](const auto&, AsyncStatus) {
        const std::lock_guard lock(completion->mutex);
        completion->done = true;
        completion->changed.notify_one();
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(completion->mutex);
    while (!completion->done) {
        if (cancelled.load() || std::chrono::steady_clock::now() >= deadline) {
            lock.unlock();
            operation.Cancel();
            throw std::runtime_error("Update operation cancelled or timed out");
        }
        completion->changed.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (cancelled.load()) {
        throw std::runtime_error("Update operation cancelled");
    }
    lock.unlock();
    return operation.GetResults();
}

void RequireSuccess(const HttpResponseMessage& response) {
    if (!response.IsSuccessStatusCode()) {
        throw std::runtime_error("Update HTTP status " +
                                 std::to_string(static_cast<int>(response.StatusCode())));
    }
}

// Match ConsolePortal.CaptureCsrf, including the cookie jar fallback used by Windows.Web.Http.
winrt::hstring CaptureCsrf(const HttpResponseMessage& response,
                           const HttpBaseProtocolFilter& filter, const Uri& base) {
    winrt::hstring cookie;
    if (response.Headers().TryGetValue(L"Set-Cookie", cookie)) {
        std::wstring lower(cookie);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        const auto at = lower.find(L"csrf-token=");
        if (at != std::wstring::npos) {
            const std::wstring original(cookie);
            const auto start = at + 11;
            return winrt::hstring(
                original.substr(start, original.find_first_of(L";, ", start) - start));
        }
    }
    for (const auto& item : filter.CookieManager().GetCookies(base)) {
        std::wstring name(item.Name());
        std::transform(name.begin(), name.end(), name.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name == L"csrf-token") {
            return item.Value();
        }
    }
    throw std::runtime_error("Device Portal did not supply a CSRF token");
}

void InstallThroughPortal(const StorageFile& package, const JsonObject& config,
                          const std::atomic<bool>& cancelled) {
    const std::wstring host(config.GetNamedString(L"host"));
    const double port = config.GetNamedNumber(L"port", 11443);
    if (host.empty() || host.find_first_of(L"/@?#\\\r\n") != std::wstring::npos ||
        !std::isfinite(port) || port < 1 || port > 65535 || port != static_cast<int>(port)) {
        throw std::runtime_error("Invalid portal.json host or port");
    }
    const std::wstring base = L"https://" + host + L":" + std::to_wstring(static_cast<int>(port));
    HttpBaseProtocolFilter filter;
    // Only this portal client trusts the console's self-signed certificate, never GitHub.
    using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;
    for (const auto error : {ChainValidationResult::Untrusted, ChainValidationResult::InvalidName,
                             ChainValidationResult::Expired, ChainValidationResult::IncompleteChain,
                             ChainValidationResult::WrongUsage}) {
        filter.IgnorableServerCertificateErrors().Append(error);
    }
    filter.AllowAutoRedirect(false); // Never forward credentials to another host.
    HttpClient http(filter);
    using namespace winrt::Windows::Security::Cryptography;
    const std::wstring credentials = std::wstring(config.GetNamedString(L"user", L"")) + L":" +
                                     std::wstring(config.GetNamedString(L"pass", L""));
    const auto auth = CryptographicBuffer::EncodeToBase64String(
        CryptographicBuffer::ConvertStringToBinary(credentials, BinaryStringEncoding::Utf8));
    http.DefaultRequestHeaders().Authorization(HttpCredentialsHeaderValue(L"Basic", auth));
    const auto probe = Await(http.GetAsync(Uri(base + L"/api/os/machinename")), cancelled);
    RequireSuccess(probe);
    const auto csrf = CaptureCsrf(probe, filter, Uri(base));
    for (int attempt = 0; attempt < 30; ++attempt) {
        const auto stream = Await(package.OpenReadAsync(), cancelled);
        HttpStreamContent part(stream);
        part.Headers().ContentType(HttpMediaTypeHeaderValue(L"application/octet-stream"));
        HttpMultipartFormDataContent content;
        content.Add(part, package.Name(), package.Name());
        HttpRequestMessage request(HttpMethod::Post(),
                                   Uri(base + L"/api/app/packagemanager/package?package=" +
                                       std::wstring(Uri::EscapeComponent(package.Name()))));
        request.Content(content);
        request.Headers().Append(L"X-CSRF-Token", csrf);
        request.Headers().Append(L"Cookie", L"CSRF-Token=" + std::wstring(csrf));
        const auto response =
            Await(http.SendRequestAsync(request), cancelled, std::chrono::minutes(10));
        if (response.IsSuccessStatusCode()) {
            return; // The portal accepted the deployment; replacement can terminate us now.
        }
        if (static_cast<int>(response.StatusCode()) != 409) {
            RequireSuccess(response);
        }
        for (int tick = 0; tick < 50; ++tick) {
            if (cancelled.load()) {
                throw std::runtime_error("Update deployment cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    throw std::runtime_error("Device Portal stayed busy after 30 deployment attempts");
}
} // namespace

Updater::Updater() : worker_([this] { Check(); }) {}

Updater::~Updater() {
    cancelled_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

double Updater::Progress() const {
    // size_ is published by the Available state before installation starts.
    return size_
               ? std::min(1.0, static_cast<double>(downloaded_.load()) / static_cast<double>(size_))
               : 0.0;
}

void Updater::Check() {
    try {
        const ScopedApartment apartment;
        HttpClient http;
        http.DefaultRequestHeaders().UserAgent().ParseAdd(L"NXbox-updater");
        const auto release =
            JsonObject::Parse(Await(http.GetStringAsync(Uri(kReleases)), cancelled_));
        const auto version =
            ParseUpdateVersion(std::wstring_view(release.GetNamedString(L"tag_name", L"")));
        const auto installed = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        const UpdateVersion current{installed.Major, installed.Minor, installed.Build,
                                    installed.Revision};
        if (!release.GetNamedBoolean(L"draft", false) &&
            !release.GetNamedBoolean(L"prerelease", false) && version && *version > current) {
            std::wstring version_text;
            for (auto component : *version) {
                if (!version_text.empty())
                    version_text += L".";
                version_text += std::to_wstring(component);
            }
            const std::wstring expected = L"NXbox_game_" + version_text + L"_x64.appx";
            for (const auto& value : release.GetNamedArray(L"assets")) {
                const auto asset = value.GetObject();
                const std::wstring url(asset.GetNamedString(L"browser_download_url", L""));
                const double size = asset.GetNamedNumber(L"size", 0);
                if (std::wstring(asset.GetNamedString(L"name", L"")) == expected &&
                    url.starts_with(kAssetPrefix) && size > 0 && size <= 4.0 * 1024 * 1024 * 1024) {
                    url_ = url;
                    size_ = static_cast<std::uint64_t>(size);
                    state_.store(UpdateState::Available);
                    Diagnostic("UI update available " + winrt::to_string(version_text));
                    return;
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI update check failed HRESULT=" + std::to_string(error.code().value));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI update check failed: ") + error.what());
    }
    state_.store(UpdateState::None); // Offline/rate-limited checks never interrupt the library.
}

void Updater::StartInstall() {
    const auto state = State();
    if (state != UpdateState::Available && state != UpdateState::Failed &&
        state != UpdateState::MissingPortal) {
        return;
    }
    if (worker_.joinable())
        worker_.join();
    downloaded_.store(0);
    state_.store(UpdateState::Downloading);
    worker_ = std::thread([this] { Install(); });
}

void Updater::Install() {
    try {
        const ScopedApartment apartment;
        const auto item = Await(
            ApplicationData::Current().LocalFolder().TryGetItemAsync(L"portal.json"), cancelled_);
        const auto credentials = item.try_as<StorageFile>();
        if (!credentials) {
            state_.store(UpdateState::MissingPortal);
            return;
        }
        const auto config =
            JsonObject::Parse(Await(FileIO::ReadTextAsync(credentials), cancelled_));
        const auto file = Await(ApplicationData::Current().TemporaryFolder().CreateFileAsync(
                                    L"NXbox-update.appx", CreationCollisionOption::ReplaceExisting),
                                cancelled_);
        SCOPE_EXIT {
            try {
                file.DeleteAsync().get();
            } catch (...) {
            }
        };
        HttpClient http;
        http.DefaultRequestHeaders().UserAgent().ParseAdd(L"NXbox-updater");
        const auto response =
            Await(http.GetAsync(Uri(url_), HttpCompletionOption::ResponseHeadersRead), cancelled_);
        RequireSuccess(response);
        const auto input = Await(response.Content().ReadAsInputStreamAsync(), cancelled_);
        const auto output = Await(file.OpenAsync(FileAccessMode::ReadWrite), cancelled_);
        // Stream to disk in bounded chunks: never hold the package in RAM.
        Buffer buffer(256 * 1024);
        std::uint64_t received = 0;
        while (true) {
            const auto chunk =
                Await(input.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial),
                      cancelled_);
            if (chunk.Length() == 0)
                break;
            received += chunk.Length();
            if (received > size_)
                throw std::runtime_error("Update asset exceeds release size");
            if (Await(output.WriteAsync(chunk), cancelled_) != chunk.Length()) {
                throw std::runtime_error("Incomplete update file write");
            }
            downloaded_.store(received);
        }
        if (received != size_)
            throw std::runtime_error("Incomplete update download");
        if (!Await(output.FlushAsync(), cancelled_))
            throw std::runtime_error("Update flush failed");
        output.Close();
        input.Close();
        state_.store(UpdateState::Installing);
        InstallThroughPortal(file, config, cancelled_);
        state_.store(UpdateState::Restarting);
        Diagnostic("UI update deployment accepted by Device Portal");
    } catch (const winrt::hresult_error& error) {
        // Do not log portal credentials, cookies or URLs supplied by portal.json.
        Diagnostic("UI update install failed HRESULT=" + std::to_string(error.code().value));
        state_.store(UpdateState::Failed);
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI update install failed: ") + error.what());
        state_.store(UpdateState::Failed);
    }
}
} // namespace EdenXbox::Ui
