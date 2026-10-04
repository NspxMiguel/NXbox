// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden headers #undef the A/W macros of the Win32 calls they
// name their methods after.
#include <windows.h>

// wingdi.h defines GetObject as a macro for GetObjectW, which would rename JsonValue::GetObject().
#ifdef GetObject
#undef GetObject
#endif

#include "eden_uwp/ui/cheats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/cheats_parse.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::Foundation::Uri;

// The feed key of CNX Updater: "cheats_config" -> url_titles and url_contents. Both zips carry the
// same files; the second is only a fallback.
constexpr const char* kTitlesUrl =
    "https://github.com/sthetix/nx-cheats-db/releases/latest/download/titles.zip";
constexpr const char* kContentsUrl =
    "https://github.com/sthetix/nx-cheats-db/releases/latest/download/contents.zip";
constexpr std::uint64_t kMaxZipBytes = 64ull << 20; // the database is about 4 MB
constexpr std::size_t kMaxCheatFileBytes = 1u << 20;
constexpr auto kCacheLifetime = std::chrono::hours(24);
constexpr const char* kModFolder = "NXboxCheats";

// Gives a worker thread a COM apartment of its own for the WinRT calls it makes.
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

// ---- HTTP ----

// GETs `url` into memory. Both waits are bounded, so a stalled connection ends in a failure
// instead of a hang. Returns an empty string on success, otherwise why it failed.
std::string HttpGetBytes(const std::string& url, std::vector<std::uint8_t>& bytes) {
    using winrt::Windows::Web::Http::HttpCompletionOption;
    try {
        winrt::Windows::Web::Http::HttpClient client;
        // A failure to add the header is not worth failing the request for.
        void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
        const auto response = AwaitBounded(
            client.GetAsync(Uri{winrt::to_hstring(url)}, HttpCompletionOption::ResponseHeadersRead),
            std::chrono::seconds(60));
        if (!response) {
            return "timed out waiting for the server";
        }
        if (!response->IsSuccessStatusCode()) {
            return fmt::format("the server answered with status {}",
                               static_cast<int>(response->StatusCode()));
        }
        const auto buffer =
            AwaitBounded(response->Content().ReadAsBufferAsync(), std::chrono::seconds(180));
        if (!buffer) {
            return "timed out during the download";
        }
        if (buffer->Length() == 0 || buffer->Length() > kMaxZipBytes) {
            return "the download has an unexpected size";
        }
        bytes.assign(buffer->data(), buffer->data() + buffer->Length());
        return {};
    } catch (const winrt::hresult_error& error) {
        return "network: " + winrt::to_string(error.message());
    }
}

// ---- Files ----

bool ReadFileBytesLocal(const fs::path& file, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

bool WriteFileAtomically(const fs::path& file, const std::string& content) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
        // A target that cannot be replaced in place: remove it and try once more.
        ec.clear();
        fs::remove(file, ec);
        ec.clear();
        fs::rename(temporary, file, ec);
        if (ec) {
            fs::remove(temporary, ec);
            return false;
        }
    }
    return true;
}

bool CacheFresh(const fs::path& file) {
    std::error_code ec;
    const auto written = fs::last_write_time(file, ec);
    if (ec) {
        return false;
    }
    const auto age = fs::file_time_type::clock::now() - written;
    return age >= age.zero() && age < kCacheLifetime;
}

// ---- The remembered choice: LocalState\cheats_<TITLEID>.json ----

using Selection = std::map<std::string, std::set<std::string>>; // build id -> names that are on

fs::path SelectionFile(const fs::path& local_state, const std::string& title_id) {
    return local_state / Widen("cheats_" + title_id + ".json");
}

Selection LoadSelection(const fs::path& file) {
    Selection selection;
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return selection;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        JsonObject root;
        if (!JsonObject::TryParse(winrt::to_hstring(text), root) || !root.HasKey(L"builds")) {
            return selection;
        }
        const IJsonValue builds_value = root.GetNamedValue(L"builds");
        if (builds_value.ValueType() != JsonValueType::Object) {
            return selection;
        }
        const JsonObject builds = builds_value.as<winrt::Windows::Data::Json::JsonObject>();
        for (const auto& pair : builds) {
            std::set<std::string>& names = selection[winrt::to_string(pair.Key())];
            if (pair.Value().ValueType() != JsonValueType::Array) {
                continue;
            }
            for (const IJsonValue& value : pair.Value().GetArray()) {
                if (value.ValueType() == JsonValueType::String) {
                    names.insert(winrt::to_string(value.GetString()));
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("CHEATS_SELECTION unreadable " + winrt::to_string(error.message()));
    }
    return selection;
}

} // namespace

// ---- The store ----

struct CheatStore::Impl {
    struct Build {
        std::string id; // 16 upper-case hex digits
        std::vector<Cheats::CheatBlock> blocks; // the master code first, when there is one
        std::vector<bool> enabled;              // parallel to `blocks`
        bool dirty = false;                     // changed, not written yet
    };

    Impl(fs::path state, std::string title) : local_state(std::move(state)), title_id(std::move(title)) {}

    fs::path local_state;
    std::string title_id;
    std::atomic<bool> cancel{false};

    mutable std::mutex mutex; // guards everything below
    CheatsPhase phase = CheatsPhase::Loading;
    std::vector<Build> builds;
    std::vector<std::string> notes;
    bool loading = false;
    bool writer_running = false;

    fs::path CacheFile() const {
        return local_state / "library" / "cheats" / "titles.zip";
    }

    fs::path ModRoot() const {
        return local_state / "eden" / "load" / Widen(title_id) / Widen(kModFolder);
    }

    fs::path CheatFile(const std::string& build_id) const {
        return ModRoot() / "cheats" / Widen(build_id + ".txt");
    }

    // The block index behind the n-th cheat the screen lists.
    static std::size_t BlockIndex(const Build& build, std::size_t cheat) {
        const std::size_t skip = !build.blocks.empty() && build.blocks.front().master ? 1 : 0;
        return cheat + skip;
    }

    // ---- Loading the database ----

    // Opens the zip held in `bytes`; `error` says why not.
    static bool OpenDatabase(const std::vector<std::uint8_t>& bytes,
                             std::vector<Cheats::ZipEntry>& entries, std::string& error) {
        return Cheats::ReadZipDirectory(bytes.data(), bytes.size(), entries, error);
    }

    // Gets the database from the cache or the network. `source` names where it came from.
    CheatsPhase FetchDatabase(std::vector<std::uint8_t>& bytes,
                              std::vector<Cheats::ZipEntry>& entries, std::string& source) {
        const fs::path cache = CacheFile();
        std::string error;
        if (CacheFresh(cache) && ReadFileBytesLocal(cache, bytes) &&
            OpenDatabase(bytes, entries, error)) {
            source = "cache";
            return CheatsPhase::Ready;
        }
        std::string reason = "no cache";
        for (const char* url : {kTitlesUrl, kContentsUrl}) {
            if (cancel.load()) {
                return CheatsPhase::Failed;
            }
            bytes.clear();
            reason = HttpGetBytes(url, bytes);
            if (reason.empty() && !OpenDatabase(bytes, entries, reason)) {
                reason = "the download is not a usable zip: " + reason;
            }
            if (reason.empty()) {
                std::error_code ec;
                fs::create_directories(cache.parent_path(), ec);
                fs::path temporary = cache;
                temporary += L".partial";
                {
                    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                }
                fs::rename(temporary, cache, ec);
                if (ec) {
                    ec.clear();
                    fs::remove(cache, ec);
                    ec.clear();
                    fs::rename(temporary, cache, ec);
                }
                source = "network";
                return CheatsPhase::Ready;
            }
            Diagnostic(fmt::format("CHEATS_FETCH failed {} {}", url, reason));
        }
        // The network failed: an older copy is better than nothing.
        bytes.clear();
        entries.clear();
        if (ReadFileBytesLocal(cache, bytes) && OpenDatabase(bytes, entries, error)) {
            source = "stale cache";
            return CheatsPhase::Ready;
        }
        return reason.rfind("network", 0) == 0 || reason.rfind("timed out", 0) == 0 ||
                       reason.rfind("the server", 0) == 0
                   ? CheatsPhase::Offline
                   : CheatsPhase::Failed;
    }

    // The names of the cheats that are in a file Eden loads, to seed the choice of a game whose
    // json is gone.
    std::set<std::string> NamesInInstalledFile(const std::string& build_id) const {
        std::set<std::string> names;
        std::vector<std::uint8_t> bytes;
        if (!ReadFileBytesLocal(CheatFile(build_id), bytes)) {
            return names;
        }
        const Cheats::ParsedCheats parsed = Cheats::ParseCheatFile(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        for (const Cheats::CheatBlock& block : parsed.blocks) {
            if (!block.master) {
                names.insert(block.name);
            }
        }
        return names;
    }

    void Load() {
        std::vector<std::uint8_t> bytes;
        std::vector<Cheats::ZipEntry> entries;
        std::string source;
        const CheatsPhase fetched = FetchDatabase(bytes, entries, source);
        if (cancel.load()) {
            const std::lock_guard<std::mutex> lock(mutex);
            loading = false;
            return;
        }
        if (fetched != CheatsPhase::Ready) {
            Diagnostic(fmt::format("CHEATS_FETCH failed title={} {}", title_id,
                                   fetched == CheatsPhase::Offline ? "offline" : "unreadable"));
            const std::lock_guard<std::mutex> lock(mutex);
            phase = fetched;
            loading = false;
            return;
        }

        const Cheats::TitleListing listing = Cheats::FindTitleFiles(entries, title_id);
        const Selection saved = LoadSelection(SelectionFile(local_state, title_id));
        std::vector<Build> found;
        int dropped = 0;
        for (const Cheats::BuildFile& file : listing.builds) {
            std::string text;
            std::string error;
            if (!Cheats::ExtractZipEntry(bytes.data(), bytes.size(), entries[file.entry],
                                         kMaxCheatFileBytes, text, error)) {
                Diagnostic(fmt::format("CHEATS_FETCH entry {} {}", file.build_id, error));
                continue;
            }
            Cheats::ParsedCheats parsed = Cheats::ParseCheatFile(text);
            dropped += parsed.dropped;
            const bool any = std::any_of(parsed.blocks.begin(), parsed.blocks.end(),
                                         [](const Cheats::CheatBlock& block) {
                                             return !block.master;
                                         });
            if (!any) {
                continue;
            }
            Build build;
            build.id = file.build_id;
            build.blocks = std::move(parsed.blocks);
            build.enabled.assign(build.blocks.size(), false);
            const auto remembered = saved.find(build.id);
            const std::set<std::string> on = remembered != saved.end()
                                                 ? remembered->second
                                                 : NamesInInstalledFile(build.id);
            for (std::size_t i = 0; i < build.blocks.size(); ++i) {
                build.enabled[i] = !build.blocks[i].master && on.count(build.blocks[i].name) > 0;
            }
            found.push_back(std::move(build));
        }
        Diagnostic(fmt::format("CHEATS_FETCH ok title={} builds={} dropped={} source={} bytes={}",
                               title_id, found.size(), dropped, source, bytes.size()));
        const std::lock_guard<std::mutex> lock(mutex);
        notes = listing.notes;
        builds = std::move(found);
        phase = builds.empty() ? CheatsPhase::NoCheats : CheatsPhase::Ready;
        loading = false;
    }

    // ---- Writing the files ----

    // Called with `mutex` held; the file is small and rewritten whole.
    Selection SelectionLocked() const {
        Selection selection;
        for (const Build& build : builds) {
            std::set<std::string>& names = selection[build.id];
            for (std::size_t i = 0; i < build.blocks.size(); ++i) {
                if (!build.blocks[i].master && build.enabled[i]) {
                    names.insert(build.blocks[i].name);
                }
            }
        }
        return selection;
    }

    void SaveSelection(const Selection& selection) const {
        try {
            JsonObject root;
            root.SetNamedValue(L"title", JsonValue::CreateStringValue(winrt::to_hstring(title_id)));
            JsonObject builds_json;
            for (const auto& [id, names] : selection) {
                JsonArray list;
                for (const std::string& name : names) {
                    list.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
                }
                builds_json.SetNamedValue(winrt::to_hstring(id), list);
            }
            root.SetNamedValue(L"builds", builds_json);
            if (!WriteFileAtomically(SelectionFile(local_state, title_id),
                                     winrt::to_string(root.Stringify()))) {
                Diagnostic("CHEATS_SELECTION could not be saved for " + title_id);
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("CHEATS_SELECTION save failed " + winrt::to_string(error.message()));
        }
    }

    // Writes the build's file, or removes it (and the empty mod folder) when no cheat is on.
    void WriteBuild(const std::string& build_id, const std::string& content, int enabled_count) {
        const fs::path file = CheatFile(build_id);
        std::error_code ec;
        if (content.empty()) {
            fs::remove(file, ec);
            // remove() only takes an empty folder away, which is what is wanted here.
            fs::remove(file.parent_path(), ec);
            fs::remove(ModRoot(), ec);
        } else if (!WriteFileAtomically(file, content)) {
            Diagnostic(fmt::format("CHEATS_INSTALL {} {} failed (cannot write the file)", title_id,
                                   build_id));
            return;
        }
        Diagnostic(fmt::format("CHEATS_INSTALL {} {} n={}", title_id, build_id, enabled_count));
    }
};

namespace {

void RunLoader(std::shared_ptr<CheatStore::Impl> impl) {
    try {
        const ScopedApartment apartment;
        impl->Load();
    } catch (const std::exception& error) {
        Diagnostic(std::string("CHEATS_FETCH failed ") + error.what());
        const std::lock_guard<std::mutex> lock(impl->mutex);
        impl->phase = CheatsPhase::Failed;
        impl->loading = false;
    } catch (...) {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        impl->phase = CheatsPhase::Failed;
        impl->loading = false;
    }
}

// Writes every changed build, one at a time, until nothing is left to write.
void RunWriter(std::shared_ptr<CheatStore::Impl> impl) {
    try {
        const ScopedApartment apartment;
        for (;;) {
            std::string build_id;
            std::string content;
            int enabled_count = 0;
            Selection selection;
            {
                const std::lock_guard<std::mutex> lock(impl->mutex);
                const auto dirty = std::find_if(impl->builds.begin(), impl->builds.end(),
                                                [](const CheatStore::Impl::Build& build) {
                                                    return build.dirty;
                                                });
                if (dirty == impl->builds.end()) {
                    impl->writer_running = false;
                    return;
                }
                dirty->dirty = false;
                build_id = dirty->id;
                content = Cheats::BuildCheatFile(dirty->blocks, dirty->enabled);
                for (std::size_t i = 0; i < dirty->blocks.size(); ++i) {
                    if (!dirty->blocks[i].master && dirty->enabled[i]) {
                        ++enabled_count;
                    }
                }
                selection = impl->SelectionLocked();
            }
            impl->WriteBuild(build_id, content, enabled_count);
            impl->SaveSelection(selection);
        }
    } catch (...) {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        impl->writer_running = false;
    }
}

} // namespace

CheatStore::CheatStore(fs::path local_state, std::string title_id)
    : impl_(std::make_shared<Impl>(std::move(local_state), std::move(title_id))) {
    impl_->loading = true;
    std::thread(RunLoader, impl_).detach();
}

CheatStore::~CheatStore() {
    // The loader and the writer hold the state alive; the loader stops at its next step and a
    // change that is still waiting to be written is written anyway.
    impl_->cancel.store(true);
}

CheatsPhase CheatStore::Phase() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->phase;
}

std::vector<std::string> CheatStore::BuildIds() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string> ids;
    for (const Impl::Build& build : impl_->builds) {
        ids.push_back(build.id);
    }
    return ids;
}

std::vector<std::string> CheatStore::Notes() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->notes;
}

std::vector<CheatItem> CheatStore::Cheats(std::size_t build) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<CheatItem> items;
    if (build >= impl_->builds.size()) {
        return items;
    }
    const Impl::Build& entry = impl_->builds[build];
    for (std::size_t i = 0; i < entry.blocks.size(); ++i) {
        if (!entry.blocks[i].master) {
            items.push_back({entry.blocks[i].name, entry.enabled[i], entry.blocks[i].opcodes});
        }
    }
    return items;
}

int CheatStore::EnabledCount(std::size_t build) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (build >= impl_->builds.size()) {
        return 0;
    }
    const Impl::Build& entry = impl_->builds[build];
    int count = 0;
    for (std::size_t i = 0; i < entry.blocks.size(); ++i) {
        if (!entry.blocks[i].master && entry.enabled[i]) {
            ++count;
        }
    }
    return count;
}

bool CheatStore::Writing() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->writer_running;
}

void CheatStore::Toggle(std::size_t build, std::size_t cheat) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (build >= impl_->builds.size()) {
        return;
    }
    Impl::Build& entry = impl_->builds[build];
    const std::size_t index = Impl::BlockIndex(entry, cheat);
    if (index >= entry.blocks.size() || entry.blocks[index].master) {
        return;
    }
    entry.enabled[index] = !entry.enabled[index];
    entry.dirty = true;
    if (!impl_->writer_running) {
        impl_->writer_running = true;
        std::thread(RunWriter, impl_).detach();
    }
}

void CheatStore::SetAll(std::size_t build, bool enabled) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (build >= impl_->builds.size()) {
        return;
    }
    Impl::Build& entry = impl_->builds[build];
    for (std::size_t i = 0; i < entry.blocks.size(); ++i) {
        if (!entry.blocks[i].master) {
            entry.enabled[i] = enabled;
        }
    }
    entry.dirty = true;
    if (!impl_->writer_running) {
        impl_->writer_running = true;
        std::thread(RunWriter, impl_).detach();
    }
}

void CheatStore::Retry() {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->loading || (impl_->phase != CheatsPhase::Offline && impl_->phase != CheatsPhase::Failed)) {
        return;
    }
    impl_->phase = CheatsPhase::Loading;
    impl_->loading = true;
    std::thread(RunLoader, impl_).detach();
}

} // namespace EdenXbox::Ui
