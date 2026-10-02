// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden file system headers #undef the A/W macros of the Win32
// calls they name their methods after.
#include <windows.h>

#include <fileapifromapp.h>

#include "eden_uwp/ui/library.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/nsz.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

// What a cached LocalState\library\<TITLEID16>.json says about a game.
struct CacheRecord {
    std::string title_id;
    std::wstring name;
    std::wstring path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::vector<std::string> nacp_names;
};

// What reading a package yields.
struct PackageInfo {
    std::uint64_t title_id = 0;
    std::wstring name;
    std::vector<std::string> nacp_names;
    std::vector<std::uint8_t> icon; // JPEG bytes, empty when the package has none
    bool keys_problem = false;      // set when the package could not be read for want of keys
};

struct ScanStats {
    int cached = 0;
    int parsed = 0;
    int skipped = 0;
    int key_problems = 0; // skipped for want of keys
};

// Gives the worker thread a COM apartment of its own for the WinRT calls it makes.
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

// Statuses that say a key is missing or wrong, as opposed to a broken or unsupported package.
bool IsKeyError(Loader::ResultStatus status) {
    switch (status) {
    case Loader::ResultStatus::ErrorMissingProductionKeyFile:
    case Loader::ResultStatus::ErrorMissingHeaderKey:
    case Loader::ResultStatus::ErrorIncorrectHeaderKey:
    case Loader::ResultStatus::ErrorMissingTitlekey:
    case Loader::ResultStatus::ErrorMissingTitlekek:
    case Loader::ResultStatus::ErrorMissingKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
        return true;
    default:
        return false;
    }
}

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return text;
}

std::string Utf8(const std::wstring& text) {
    return winrt::to_string(text);
}

std::int64_t ModifiedSeconds(const fs::path& file) {
    std::error_code error;
    const auto stamp = fs::last_write_time(file, error);
    if (error) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExFromAppW(file.c_str(), GetFileExInfoStandard, &data)) {
            return 0;
        }
        const auto ticks = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
        return static_cast<std::int64_t>(ticks / 10000000);
    }
    return std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
}

bool ReadRecord(const fs::path& json_file, CacheRecord& record) {
    try {
        std::ifstream in(json_file, std::ios::binary);
        if (!in) {
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        JsonObject object;
        if (!JsonObject::TryParse(winrt::to_hstring(text), object)) {
            return false;
        }
        record.title_id = winrt::to_string(object.GetNamedString(L"title_id"));
        record.name = object.GetNamedString(L"name").c_str();
        record.path = object.GetNamedString(L"path").c_str();
        record.size = static_cast<std::uint64_t>(object.GetNamedNumber(L"size"));
        record.mtime = static_cast<std::int64_t>(object.GetNamedNumber(L"mtime"));
        // A record from before the names were cached has no such array: it is rebuilt, which is
        // how the names get into the cache.
        const winrt::Windows::Data::Json::JsonArray names = object.GetNamedArray(L"nacp_names");
        for (uint32_t i = 0; i < names.Size(); ++i) {
            record.nacp_names.push_back(winrt::to_string(names.GetAt(i).GetString()));
        }
        return true;
    } catch (const winrt::hresult_error&) {
        return false; // a field is missing or has the wrong type: the entry is rebuilt
    }
}

bool WriteRecord(const fs::path& json_file, const GameEntry& game) {
    try {
        JsonObject object;
        object.SetNamedValue(L"title_id",
                             JsonValue::CreateStringValue(winrt::to_hstring(game.title_id)));
        object.SetNamedValue(L"name", JsonValue::CreateStringValue(game.name));
        object.SetNamedValue(L"path", JsonValue::CreateStringValue(game.path.wstring()));
        object.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(game.size)));
        object.SetNamedValue(L"mtime",
                             JsonValue::CreateNumberValue(static_cast<double>(game.mtime)));
        winrt::Windows::Data::Json::JsonArray names;
        for (const std::string& name : game.nacp_names) {
            names.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
        }
        object.SetNamedValue(L"nacp_names", names);
        std::ofstream out(json_file, std::ios::binary | std::ios::trunc);
        out << winrt::to_string(object.Stringify());
        return static_cast<bool>(out);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// The control NCA of the base game, which holds control.nacp and the icons.
std::shared_ptr<FileSys::NCA> FindControlNca(const FileSys::NSP& nsp, std::uint64_t title_id) {
    if (auto control = nsp.GetNCA(title_id, FileSys::ContentRecordType::Control)) {
        return control;
    }
    // Multi-program titles can keep their control data under another title ID.
    for (const auto& title : nsp.GetNCAs()) {
        for (const auto& entry : title.second) {
            const auto& [title_type, record_type] = entry.first;
            if (title_type == FileSys::TitleType::Application &&
                record_type == FileSys::ContentRecordType::Control) {
                return entry.second;
            }
        }
    }
    return nullptr;
}

// Reads a package's title ID, name and icon. Returns why the package cannot be used, or an empty
// string on success.
std::string ReadPackage(FileSys::RealVfsFilesystem& vfs, const fs::path& file, PackageInfo& info) {
    const FileSys::VirtualFile source =
        vfs.OpenFile(Common::FS::PathToUTF8String(file), FileSys::OpenMode::Read);
    if (!source) {
        return "cannot open the file";
    }

    std::shared_ptr<FileSys::NSP> nsp;
    if (Lowercase(file.extension().wstring()) == L".xci") {
        const FileSys::XCI xci(source);
        if (xci.GetStatus() != Loader::ResultStatus::Success) {
            return "XCI: " + Loader::GetResultStatusString(xci.GetStatus());
        }
        nsp = xci.GetSecurePartitionNSP();
    } else {
        nsp = std::make_shared<FileSys::NSP>(source);
    }
    if (!nsp) {
        return "no secure partition";
    }
    if (nsp->GetStatus() != Loader::ResultStatus::Success) {
        return "NSP: " + Loader::GetResultStatusString(nsp->GetStatus());
    }
    if (nsp->IsExtractedType()) {
        return "extracted NSP, it has no control data";
    }

    info.title_id = nsp->GetProgramTitleID();
    if (info.title_id == 0) {
        // Without the keys no NCA opens, and the status kept for the program says exactly that.
        const Loader::ResultStatus status = nsp->GetProgramStatus();
        info.keys_problem = IsKeyError(status);
        return status == Loader::ResultStatus::Success
                   ? "no base-game program NCA (update or DLC only?)"
                   : "NSP: " + Loader::GetResultStatusString(status);
    }
    const std::shared_ptr<FileSys::NCA> control = FindControlNca(*nsp, info.title_id);
    if (!control) {
        return "no control NCA";
    }
    if (control->GetStatus() != Loader::ResultStatus::Success) {
        info.keys_problem = IsKeyError(control->GetStatus());
        return "control NCA: " + Loader::GetResultStatusString(control->GetStatus());
    }
    const FileSys::VirtualFile romfs = control->GetRomFS();
    if (!romfs) {
        return "control NCA has no RomFS";
    }
    const FileSys::VirtualDir content = FileSys::ExtractRomFS(romfs);
    if (!content) {
        return "control RomFS is unreadable";
    }
    FileSys::VirtualFile nacp_file = content->GetFile("control.nacp");
    if (!nacp_file) {
        nacp_file = content->GetFile("Control.nacp");
    }
    if (!nacp_file) {
        return "no control.nacp";
    }
    const FileSys::NACP nacp(nacp_file);
    info.name = winrt::to_hstring(nacp.GetApplicationName()).c_str();
    info.nacp_names = nacp.GetApplicationNames();
    if (info.name.empty()) {
        info.name = file.stem().wstring();
    }

    // The Switch's own square icon, a JPEG next to control.nacp. Any other language will do when
    // the American English one is missing.
    FileSys::VirtualFile icon = content->GetFile("icon_AmericanEnglish.dat");
    if (!icon) {
        for (const FileSys::VirtualFile& candidate : content->GetFiles()) {
            const std::string name = candidate->GetName();
            if (name.starts_with("icon_") && name.ends_with(".dat")) {
                icon = candidate;
                break;
            }
        }
    }
    if (icon) {
        info.icon = icon->ReadAllBytes();
    }
    return {};
}

// One game from one file: the cache when it still matches, otherwise the package itself.
std::optional<GameEntry> LoadGame(FileSys::RealVfsFilesystem& vfs, const fs::path& library_dir,
                                  const std::map<std::wstring, CacheRecord>& cached,
                                  const fs::path& file, ScanStats& stats) {
    GameEntry game;
    game.path = file;
    game.size = Common::FS::GetSize(file);
    game.mtime = ModifiedSeconds(file);

    const auto hit = cached.find(file.wstring());
    if (hit != cached.end() && game.size != 0 && hit->second.size == game.size &&
        hit->second.mtime == game.mtime) {
        game.title_id = hit->second.title_id;
        game.name = hit->second.name;
        game.nacp_names = hit->second.nacp_names;
        const fs::path icon = library_dir / (game.title_id + ".jpg");
        std::error_code icon_error;
        if (fs::exists(icon, icon_error)) {
            game.icon = icon;
        }
        ++stats.cached;
        return game;
    }

    PackageInfo info;
    std::string reason;
    try {
        reason = ReadPackage(vfs, file, info);
    } catch (const std::exception& failure) {
        reason = failure.what();
    }
    if (!reason.empty()) {
        Diagnostic("UI library skip " + Utf8(file.filename().wstring()) + ": " + reason);
        ++stats.skipped;
        stats.key_problems += info.keys_problem ? 1 : 0;
        return std::nullopt;
    }

    game.title_id = fmt::format("{:016X}", info.title_id);
    game.name = info.name;
    game.nacp_names = info.nacp_names;
    if (!info.icon.empty()) {
        const fs::path icon = library_dir / (game.title_id + ".jpg");
        std::ofstream out(icon, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(info.icon.data()),
                  static_cast<std::streamsize>(info.icon.size()));
        if (out) {
            game.icon = icon;
        }
    }
    if (!WriteRecord(library_dir / (game.title_id + ".json"), game)) {
        Diagnostic("UI library cache write failed for " + game.title_id);
    }
    ++stats.parsed;
    return game;
}

// Converts one .nsz into the .nsp at `nsp_path`. The converter writes to "<name>.nsp.partial",
// which is renamed only when the conversion is complete, so a half-written .nsp never exists under
// its real name. Returns why it failed, or an empty string when the .nsp is complete.
std::string ConvertNsz(FileSys::RealVfsFilesystem& vfs, const fs::path& nsz_path,
                       const fs::path& nsp_path,
                       const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                       const std::atomic<bool>& cancel) {
    fs::path partial = nsp_path;
    partial += L".partial";
    std::string error;
    bool converted = false;
    try {
        // Both files are closed at the end of this block, before the rename or the delete.
        const FileSys::VirtualFile source =
            vfs.OpenFile(Common::FS::PathToUTF8String(nsz_path), FileSys::OpenMode::Read);
        if (!source) {
            return "cannot open the file";
        }
        if (!FileSys::IsNsz(source)) {
            return "not an NSZ package";
        }
        const FileSys::VirtualFile output =
            vfs.CreateFile(Common::FS::PathToUTF8String(partial), FileSys::OpenMode::ReadWrite);
        if (!output) {
            void(Common::FS::RemoveFile(partial)); // an empty file may have been left behind
            return "cannot create " + Utf8(partial.filename().wstring());
        }
        converted = FileSys::ConvertNszToNsp(source, output, progress, &error, &cancel);
    } catch (const std::exception& failure) {
        // The scan goes on with the other packages; the partial output is removed below.
        converted = false;
        error = failure.what();
    }
    if (!converted) {
        // The converter leaves its output undefined after a failure.
        void(Common::FS::RemoveFile(partial));
        return error.empty() ? "the conversion failed" : error;
    }
    if (Common::FS::GetSize(partial) == 0) {
        void(Common::FS::RemoveFile(partial));
        return "the converter wrote an empty file";
    }
    if (!Common::FS::RenameFile(partial, nsp_path)) {
        const std::string reason = "cannot rename the output";
        void(Common::FS::RemoveFile(partial));
        return reason;
    }
    return {};
}
} // namespace

LibraryScan::LibraryScan(std::filesystem::path local_state)
    : local_state_(std::move(local_state)) {
    worker_ = std::thread([this] { Run(); });
}

LibraryScan::~LibraryScan() {
    cancel_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool LibraryScan::Finished() const {
    return finished_.load(std::memory_order_acquire);
}

int LibraryScan::Done() const {
    return done_.load();
}

int LibraryScan::Total() const {
    return total_.load();
}

bool LibraryScan::KeysMissing() const {
    return keys_missing_.load();
}

LibraryScan::Conversion LibraryScan::CurrentConversion() const {
    const std::lock_guard<std::mutex> lock(conversion_mutex_);
    return conversion_;
}

std::vector<GameEntry> LibraryScan::Take() {
    return std::move(games_);
}

void LibraryScan::ConvertCompressed(const std::vector<fs::path>& compressed,
                                    std::vector<fs::path>& packages) {
    // An .nsz with an .nsp next to it has been converted before (or the player copied both): the
    // .nsp is the package, and nothing is deleted.
    std::vector<std::pair<fs::path, fs::path>> pending; // .nsz and the .nsp it becomes
    std::optional<std::vector<fs::path>> external; // removable NXbox\games folders, looked up once
    for (const fs::path& nsz : compressed) {
        fs::path nsp = nsz;
        nsp.replace_extension(L".nsp");
        if (!external) {
            external = ExternalGameFolders();
        }
        fs::path existing;
        if (Common::FS::Exists(nsp)) {
            existing = nsp;
        }
        for (const fs::path& folder : *external) {
            if (existing.empty() && Common::FS::Exists(folder / nsp.filename())) {
                existing = folder / nsp.filename();
            }
        }
        if (!existing.empty()) {
            Diagnostic("UI nsz convert " + Utf8(nsz.filename().wstring()) + " skip " +
                       Utf8(existing.wstring()) + " already exists");
            continue;
        }
        // The .nsp runs about 1.4 times its .nsz; 1.5 leaves a margin. The console's internal
        // storage holds only a few GB, so a big game is converted onto a removable drive with
        // room, and when no place has room the conversion is not attempted at all: a doomed
        // attempt ran for minutes on every library open and failed with "disk full" each time.
        const std::uint64_t needed = Common::FS::GetSize(nsz) * 3 / 2;
        const auto free_in = [](const fs::path& folder) { return FreeSpace(folder); };
        const auto free_here = free_in(nsz.parent_path());
        if (!free_here || *free_here < needed) {
            bool placed = false;
            for (const fs::path& folder : *external) {
                const auto free_there = free_in(folder);
                if (folder != nsz.parent_path() && free_there && *free_there >= needed) {
                    nsp = folder / nsp.filename();
                    placed = true;
                    Diagnostic("UI nsz convert " + Utf8(nsz.filename().wstring()) +
                               " to removable drive " + Utf8(folder.wstring()));
                    break;
                }
            }
            if (!placed) {
                Diagnostic(fmt::format("UI nsz convert {} skip: needs {} MiB free, has {} MiB",
                                       Utf8(nsz.filename().wstring()), needed >> 20,
                                       free_here.value_or(0) >> 20));
                continue;
            }
        }
        // A conversion that ran out of space is not retried until the target has gained room:
        // the free-space figures overstate what the console lets an app write.
        const fs::path marker = local_state_ / "library" / (nsz.stem().wstring() + L".nospace");
        std::uint64_t free_at_failure = 0;
        if (std::ifstream in{marker}; in >> free_at_failure) {
            const auto free_now = free_in(nsp.parent_path()).value_or(0);
            if (free_now < free_at_failure + (1ull << 30)) {
                Diagnostic(fmt::format("UI nsz convert {} skip: ran out of space before with {} "
                                       "MiB free, {} MiB now",
                                       Utf8(nsz.filename().wstring()), free_at_failure >> 20,
                                       free_now >> 20));
                continue;
            }
            std::error_code ec;
            fs::remove(marker, ec);
        }
        pending.emplace_back(nsz, nsp);
    }
    if (pending.empty()) {
        return;
    }

    FileSys::RealVfsFilesystem vfs;
    int index = 0;
    for (const auto& entry : pending) {
        if (cancel_.load()) {
            break;
        }
        const fs::path& nsz = entry.first;
        const fs::path& nsp = entry.second;
        const std::string file_name = Utf8(nsz.filename().wstring());
        ++index;
        {
            const std::lock_guard<std::mutex> lock(conversion_mutex_);
            conversion_ = Conversion{true, nsz.stem().wstring(), index,
                                     static_cast<int>(pending.size()), 0, 0};
        }
        const std::string failure = ConvertNsz(
            vfs, nsz, nsp,
            [this](std::uint64_t written, std::uint64_t size) {
                const std::lock_guard<std::mutex> lock(conversion_mutex_);
                conversion_.done = written;
                conversion_.total = size;
            },
            cancel_);
        if (!failure.empty()) {
            Diagnostic("UI nsz convert " + file_name + " fail " + failure);
            if (failure.find("disk full") != std::string::npos) {
                std::ofstream(local_state_ / "library" / (nsz.stem().wstring() + L".nospace"))
                    << FreeSpace(nsp.parent_path()).value_or(0) << '\n';
            }
            continue;
        }
        Diagnostic("UI nsz convert " + file_name + " ok");
        // The .nsz goes only now that its .nsp is complete and in place.
        if (!Common::FS::RemoveFile(nsz)) {
            Diagnostic("UI nsz convert " + file_name + " ok, but the .nsz could not be removed");
        }
        packages.push_back(nsp);
    }
    const std::lock_guard<std::mutex> lock(conversion_mutex_);
    conversion_ = Conversion{};
}

std::vector<GameEntry> LibraryScan::Scan() {
    const auto started = std::chrono::steady_clock::now();
    const fs::path games_dir = local_state_ / "games";
    const fs::path library_dir = local_state_ / "library";

    std::vector<fs::path> packages;   // .nsp and .xci
    std::vector<fs::path> compressed; // .nsz
    {
        std::error_code error;
        fs::directory_iterator iterator(games_dir, error);
        if (error) {
            Diagnostic("UI library scan: no local games folder (" + error.message() + ")");
        }
        for (const fs::directory_entry& entry : iterator) {
            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error)) {
                continue;
            }
            const std::wstring extension = Lowercase(entry.path().extension().wstring());
            if (extension == L".nsp" || extension == L".xci") {
                packages.push_back(entry.path());
            } else if (extension == L".nsz") {
                compressed.push_back(entry.path());
            }
        }
    }
    for (const auto& file : ListExternalGames()) {
        if (Lowercase(file.extension().wstring()) == L".nsz") {
            compressed.push_back(file);
        } else {
            packages.push_back(file);
        }
    }
    std::sort(compressed.begin(), compressed.end());
    ConvertCompressed(compressed, packages);
    std::sort(packages.begin(), packages.end());
    total_.store(static_cast<int>(packages.size()));

    // The cache is indexed by package path: the title ID is only known after parsing.
    std::error_code create_error;
    void(fs::create_directories(library_dir, create_error));
    std::map<std::wstring, CacheRecord> cached;
    {
        std::error_code error;
        fs::directory_iterator iterator(library_dir, error);
        if (!error) {
            for (const fs::directory_entry& entry : iterator) {
                CacheRecord record;
                // <TITLEID16>.json only: the folder also holds <TITLEID16>.mods.json and the art
                // index, which is large and not a game record.
                if (Lowercase(entry.path().extension().wstring()) == L".json" &&
                    entry.path().stem().wstring().size() == 16 &&
                    ReadRecord(entry.path(), record)) {
                    cached.emplace(record.path, record);
                }
            }
        }
    }

    FileSys::RealVfsFilesystem vfs;
    ScanStats stats;
    std::set<std::string> seen;
    std::vector<GameEntry> games;
    for (const fs::path& file : packages) {
        if (cancel_.load()) {
            break;
        }
        std::optional<GameEntry> game = LoadGame(vfs, library_dir, cached, file, stats);
        if (game) {
            // The cache files are named by title ID, so two packages of one title cannot coexist.
            if (seen.insert(game->title_id).second) {
                games.push_back(std::move(*game));
            } else {
                Diagnostic("UI library skip " + Utf8(file.filename().wstring()) +
                           ": duplicate of title " + game->title_id);
                ++stats.skipped;
            }
        }
        done_.fetch_add(1);
    }

    std::sort(games.begin(), games.end(), [](const GameEntry& left, const GameEntry& right) {
        const int order = _wcsicmp(left.name.c_str(), right.name.c_str());
        return order != 0 ? order < 0 : left.path < right.path;
    });
    keys_missing_.store(games.empty() && stats.key_problems > 0);
    Diagnostic(fmt::format(
        "UI library scan games={} cached={} parsed={} skipped={} keys={} ms={}", games.size(),
        stats.cached, stats.parsed, stats.skipped, stats.key_problems,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started)
            .count()));
    return games;
}

void LibraryScan::Run() {
    try {
        const ScopedApartment apartment;
        games_ = Scan();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI library scan failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI library scan failed ") + error.what());
    }
    finished_.store(true, std::memory_order_release);
}

} // namespace EdenXbox::Ui
