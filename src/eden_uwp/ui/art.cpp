// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "eden_uwp/ui/art.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "eden_uwp/diagnostic.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;

constexpr const char* kIndexUrl =
    "https://raw.githubusercontent.com/NspxMiguel/NXbox/main/dist/art/eshop-art.json";
constexpr std::uint32_t kMaxIndexBytes = 32u << 20;
constexpr std::uint32_t kMaxBannerBytes = 16u << 20;
constexpr auto kIndexMaxAge = std::chrono::hours(24 * 7);

class ScopedApartment {
public:
    ScopedApartment() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    ~ScopedApartment() {
        winrt::uninit_apartment();
    }
    ScopedApartment(const ScopedApartment&) = delete;
    ScopedApartment& operator=(const ScopedApartment&) = delete;
};

// GETs a body into `bytes`. False, with a log line, on a network error or a status other than 200.
bool HttpGet(const std::string& url, std::uint32_t limit, std::vector<std::uint8_t>& bytes) {
    try {
        winrt::Windows::Web::Http::HttpClient client;
        void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
        const auto response =
            client.GetAsync(winrt::Windows::Foundation::Uri{winrt::to_hstring(url)}).get();
        if (response.StatusCode() != winrt::Windows::Web::Http::HttpStatusCode::Ok) {
            Diagnostic(fmt::format("UI art http {} status={}", url,
                                   static_cast<int>(response.StatusCode())));
            return false;
        }
        const auto buffer = response.Content().ReadAsBufferAsync().get();
        if (buffer.Length() == 0 || buffer.Length() > limit) {
            Diagnostic(fmt::format("UI art http {} rejected size={}", url, buffer.Length()));
            return false;
        }
        bytes.assign(buffer.data(), buffer.data() + buffer.Length());
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI art http " + url + " failed " + winrt::to_string(error.message()));
        return false;
    }
}

bool ReadText(const fs::path& file, std::string& text) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !text.empty();
}

// Written under a temporary name and renamed: a full disk used to leave an empty or truncated
// file that later runs trusted (the error only shows when the stream is closed).
bool WriteBytes(const fs::path& file, const std::vector<std::uint8_t>& bytes) {
    fs::path partial = file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(partial, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(partial, file, ec);
    if (ec) {
        fs::remove(partial, ec);
        return false;
    }
    return true;
}

// The text of the JSON string that follows `"key"` and a colon, starting the search at `from`.
// The index is a flat, machine-written file, so a scan is enough and keeps the memory low (a
// parsed tree of thousands of titles would take far more than the text). Returns npos when the
// key is not there or is not followed by a string.
std::size_t FindStringAfterKey(const std::string& text, const std::string& key, std::size_t from,
                               std::string& value, std::size_t* after = nullptr) {
    const std::string quoted = "\"" + key + "\"";
    const auto skip = [&](std::size_t pos) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' ||
                                     text[pos] == '\t')) {
            ++pos;
        }
        return pos;
    };
    std::size_t search = from;
    for (;;) {
        std::size_t at = text.find(quoted, search);
        if (at == std::string::npos) {
            return std::string::npos;
        }
        search = at + quoted.size(); // a match that is not a key is skipped
        at = skip(search);
        if (at >= text.size() || text[at] != ':') {
            continue;
        }
        at = skip(at + 1);
        if (at < text.size() && text[at] == '[') {
            at = skip(at + 1); // the first element of an array
        }
        if (at >= text.size() || text[at] != '"') {
            continue;
        }
        const std::size_t end = text.find('"', at + 1);
        if (end == std::string::npos) {
            return std::string::npos;
        }
        value = text.substr(at + 1, end - at - 1);
        if (after != nullptr) {
            *after = end + 1;
        }
        return at;
    }
}

} // namespace

struct BannerSource::Impl : std::enable_shared_from_this<BannerSource::Impl> {
    explicit Impl(fs::path local_state_in, bool fetch_icon_in)
        : fetch_icon(fetch_icon_in), local_state(std::move(local_state_in)) {}

    bool fetch_icon = false;

    fs::path local_state;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::map<std::string, BannerState> states;
    std::vector<std::string> stack; // pending requests, the newest last
    std::string index;              // the index text; empty when there is none
    std::string base;
    std::string extension;

    fs::path BannerFile(const std::string& title_id) const {
        return local_state / "library" / (title_id + ".banner.jpg");
    }

    void LoadIndex() {
        const fs::path file = local_state / "library" / "eshop-art.json";
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        bool fresh = false;
        if (fs::exists(file, ec)) {
            const auto stamp = fs::last_write_time(file, ec);
            // An empty file is what a full disk left behind; it is never fresh.
            if (!ec && fs::file_size(file, ec) > 0 && !ec) {
                fresh = fs::file_time_type::clock::now() - stamp < kIndexMaxAge;
            }
        }
        std::string text;
        if (!fresh) {
            std::vector<std::uint8_t> bytes;
            if (HttpGet(kIndexUrl, kMaxIndexBytes, bytes) && WriteBytes(file, bytes)) {
                Diagnostic(fmt::format("UI art index downloaded {} bytes", bytes.size()));
            }
        }
        if (!ReadText(file, text)) {
            Diagnostic("UI art index unavailable, the hero keeps the icon gradient");
            return;
        }
        std::string found_base;
        std::string found_extension;
        if (FindStringAfterKey(text, "base", 0, found_base) == std::string::npos ||
            FindStringAfterKey(text, "ext", 0, found_extension) == std::string::npos) {
            Diagnostic("UI art index has no base or ext");
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex);
        base = std::move(found_base);
        extension = std::move(found_extension);
        index = std::move(text);
        Diagnostic(fmt::format("UI art index ready ({} bytes)", index.size()));
    }

    // The banner id of a title, or empty when the index does not know it.
    std::string BannerId(const std::string& title_id) {
        const std::lock_guard<std::mutex> lock(mutex);
        if (index.empty()) {
            return {};
        }
        // "titles":{ ... "<TITLEID>":["<banner>","<icon>"] ... }
        const std::size_t titles = index.find("\"titles\"");
        std::string id;
        if (FindStringAfterKey(index, title_id, titles == std::string::npos ? 0 : titles, id) ==
            std::string::npos) {
            return {};
        }
        return id;
    }

    void FetchIcon(const std::string& title_id) {
        const fs::path file = local_state / "library" / (title_id + ".jpg");
        std::error_code ec;
        if (fs::exists(file, ec) && fs::file_size(file, ec) > 0)
            return;
        std::string id;
        std::size_t after = 0;
        if (FindStringAfterKey(index, title_id, 0, id, &after) == std::string::npos)
            return;
        // The second string in the same array is the icon, never the next title's banner.
        const auto comma = index.find_first_not_of(" \r\n\t", after);
        if (comma == std::string::npos || index[comma] != ',')
            return;
        const auto begin = index.find_first_not_of(" \r\n\t", comma + 1);
        if (begin == std::string::npos || index[begin] != '"')
            return;
        const auto end = index.find('"', begin + 1);
        if (end == std::string::npos || end == begin + 1)
            return;
        id = index.substr(begin + 1, end - begin - 1);
        std::vector<std::uint8_t> bytes;
        if (HttpGet(base + id + extension, kMaxBannerBytes, bytes) && WriteBytes(file, bytes)) {
            Diagnostic("UI art icon " + title_id + " cached");
        }
    }

    BannerState Fetch(const std::string& title_id) {
        const fs::path file = BannerFile(title_id);
        std::error_code ec;
        if (fs::exists(file, ec) && fs::file_size(file, ec) > 0) {
            return BannerState::Ready; // cached by an earlier run
        }
        const std::string id = BannerId(title_id);
        if (id.empty()) {
            Diagnostic("UI art no banner for " + title_id);
            return BannerState::None;
        }
        std::vector<std::uint8_t> bytes;
        if (!HttpGet(base + id + extension, kMaxBannerBytes, bytes)) {
            return BannerState::None;
        }
        // Written under a temporary name and renamed, so a half-written file never counts.
        const fs::path partial = file.parent_path() / (title_id + ".banner.partial");
        if (!WriteBytes(partial, bytes)) {
            fs::remove(partial, ec);
            return BannerState::None;
        }
        fs::rename(partial, file, ec);
        if (ec) {
            fs::remove(partial, ec);
            return BannerState::None;
        }
        Diagnostic(fmt::format("UI art banner {} cached {} bytes", title_id, bytes.size()));
        return BannerState::Ready;
    }

    void Loop() {
        try {
            const ScopedApartment apartment;
            LoadIndex();
            for (;;) {
                std::string title_id;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [&] { return stop || !stack.empty(); });
                    if (stop) {
                        return;
                    }
                    title_id = std::move(stack.back());
                    stack.pop_back();
                }
                BannerState result = BannerState::None;
                try {
                    if (fetch_icon)
                        FetchIcon(title_id);
                    result = Fetch(title_id);
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("UI art fetch failed " + winrt::to_string(error.message()));
                } catch (const std::exception& error) {
                    Diagnostic(std::string("UI art fetch failed ") + error.what());
                }
                const std::lock_guard<std::mutex> lock(mutex);
                states[title_id] = result;
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI art worker failed " + winrt::to_string(error.message()));
        }
    }
};

BannerSource::BannerSource(fs::path local_state, bool fetch_icon)
    : impl_(std::make_shared<Impl>(std::move(local_state), fetch_icon)) {
    // The thread owns a reference, so the screen can go away while a download finishes.
    std::thread([impl = impl_] { impl->Loop(); }).detach();
}

BannerSource::~BannerSource() {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->wake.notify_all();
}

void BannerSource::Request(const std::string& title_id) {
    if (title_id.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->states.find(title_id);
        if (found != impl_->states.end()) {
            if (found->second != BannerState::Pending) {
                return;
            }
            // Pending already: move it to the top of the stack, it is wanted now.
            for (auto it = impl_->stack.begin(); it != impl_->stack.end(); ++it) {
                if (*it == title_id) {
                    impl_->stack.erase(it);
                    break;
                }
            }
        } else {
            impl_->states.emplace(title_id, BannerState::Pending);
        }
        impl_->stack.push_back(title_id);
    }
    impl_->wake.notify_one();
}

BannerState BannerSource::Get(const std::string& title_id, fs::path& file) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->states.find(title_id);
    if (found == impl_->states.end()) {
        return BannerState::Pending;
    }
    if (found->second == BannerState::Ready) {
        file = impl_->BannerFile(title_id);
    }
    return found->second;
}

void BannerSource::Discard(const std::string& title_id) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->states[title_id] = BannerState::None;
    std::error_code ec;
    fs::remove(impl_->BannerFile(title_id), ec);
}

} // namespace EdenXbox::Ui
