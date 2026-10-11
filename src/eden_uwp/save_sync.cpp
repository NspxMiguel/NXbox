// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save sync backend, see save_sync.h. Every rule below mirrors SwitchSaveSync (core/drive.c,
// core/cloud.c, core/syncjob.c, core/syncstate.c, core/oauth.c); the comments name the source so a
// change there is easy to follow here.
#include "eden_uwp/save_sync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/hle/service/acc/profile_manager.h"
#include "eden_uwp/diagnostic.h"

namespace EdenXbox::SaveSync {
namespace {

namespace fs = std::filesystem;
namespace wj = winrt::Windows::Data::Json;
namespace wf = winrt::Windows::Foundation;
namespace wh = winrt::Windows::Web::Http;
namespace whh = winrt::Windows::Web::Http::Headers;
namespace wst = winrt::Windows::Storage::Streams;

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

// core/config.h.example: DRIVE_APP_FOLDER_NAME, GOOGLE_OAUTH_SCOPE.
constexpr const char* RootFolderName = "Nintendo Switch Saves";
constexpr const char* DriveScope = "https://www.googleapis.com/auth/drive.file";
// core/drive.c: DRIVE_FOLDER_MIME. cloud_upload_tree() uploads every file as octet-stream.
constexpr const char* FolderMime = "application/vnd.google-apps.folder";
constexpr const char* FileMime = "application/octet-stream";
constexpr const char* DeviceCodeUrl = "https://oauth2.googleapis.com/device/code";
constexpr const char* TokenUrl = "https://oauth2.googleapis.com/token";
constexpr const char* FilesUrl = "https://www.googleapis.com/drive/v3/files";
constexpr const char* UploadUrl = "https://www.googleapis.com/upload/drive/v3/files";
constexpr const char* MultipartBoundary = "NXboxSaveSyncBoundary7f3a91";

// Eden's own bookkeeping file inside every save folder (FileSys::GetSaveDataSizeFileName(),
// savedata_factory.h). It is not part of the game's save, so it is never hashed, uploaded or
// deleted. Eden hides it from the guest as well (fs_i_directory.h).
constexpr const char* SaveSizeFileName = ".yuzu_save_size";

// Buffer sizes SwitchSaveSync sanitizes and truncates folder names with (core/syncjob.c).
constexpr std::size_t GameNameBuffer = 0x201;   // the game-name buffer in cloud_folder_path()
constexpr std::size_t OwnerNameBuffer = 0x41;   // CLOUD_OWNER_MAX
constexpr std::size_t FolderPathBuffer = 0x220; // the path buffer cloud_title_folder() passes in
constexpr std::size_t NacpLanguageCount = 16;   // NacpStruct.lang[16], what the Switch reads
constexpr std::size_t NacpNameBytes = 0x200;

// core/drive.c: above this size the multipart upload is not used.
constexpr std::uint64_t MultipartMax = 4ull * 1024 * 1024;
// Whole files are held in memory for HTTP; real saves are a few MB.
constexpr std::uint64_t MaxFileBytes = 256ull * 1024 * 1024;

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------

struct SyncError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct NotSignedInError : SyncError {
    using SyncError::SyncError;
};
struct NotConfiguredError : SyncError {
    using SyncError::SyncError;
};

void Log(const std::string& line) {
    Diagnostic("SAVESYNC " + line);
}

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::string Hex16(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

bool ParseHex64(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    out = value;
    return true;
}

std::string LowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return LowerAscii(a) == LowerAscii(b);
}

bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string Trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && IsBlank(text[first])) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && IsBlank(text[last - 1])) {
        --last;
    }
    return text.substr(first, last - first);
}

// Paths: std::filesystem::path(std::string) reads the string in the ANSI code page on Windows, so
// every name that came from the cloud or from a game goes through UTF-8 explicitly.
fs::path PathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(std::wstring_view(winrt::to_hstring(utf8)));
#else
    return fs::path(utf8);
#endif
}

std::string Utf8FromPath(const fs::path& path) {
#ifdef _WIN32
    return winrt::to_string(std::wstring_view(path.native()));
#else
    return path.string();
#endif
}

std::int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// file_time_type has an implementation-defined epoch (1601 on MSVC). The offset between the two
// clocks is measured once per call, which is accurate to well under a second.
std::int64_t UnixFromFileTime(fs::file_time_type stamp) {
    const auto offset = std::chrono::system_clock::now().time_since_epoch() -
                        fs::file_time_type::clock::now().time_since_epoch();
    const auto shifted = stamp.time_since_epoch() + offset;
    return std::chrono::duration_cast<std::chrono::seconds>(shifted).count();
}

bool ReadDigits(const std::string& text, std::size_t pos, std::size_t count, int& out) {
    if (pos + count > text.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    if (month <= 2) {
        year -= 1;
    }
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// Drive's modifiedTime, RFC 3339 ("2026-10-01T12:34:56.789Z"). 0 when it cannot be read.
std::int64_t ParseRfc3339(const std::string& text) {
    if (text.size() < 20) {
        return 0;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    const bool shape_ok = ReadDigits(text, 0, 4, year) && text[4] == '-' &&
                          ReadDigits(text, 5, 2, month) && text[7] == '-' &&
                          ReadDigits(text, 8, 2, day) &&
                          (text[10] == 'T' || text[10] == 't' || text[10] == ' ') &&
                          ReadDigits(text, 11, 2, hour) && text[13] == ':' &&
                          ReadDigits(text, 14, 2, minute) && text[16] == ':' &&
                          ReadDigits(text, 17, 2, second);
    if (!shape_ok || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    std::size_t pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
    }
    std::int64_t offset_seconds = 0;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        int offset_hours = 0;
        int offset_minutes = 0;
        if (pos + 6 > text.size() || text[pos + 3] != ':' ||
            !ReadDigits(text, pos + 1, 2, offset_hours) ||
            !ReadDigits(text, pos + 4, 2, offset_minutes)) {
            return 0;
        }
        offset_seconds = static_cast<std::int64_t>(offset_hours) * 3600 + offset_minutes * 60;
        if (text[pos] == '-') {
            offset_seconds = -offset_seconds;
        }
    }
    const std::int64_t days =
        DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
}

// RFC 3986 unreserved characters pass through, everything else is %XX.
std::string UrlEncode(const std::string& in) {
    static const char HexDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(HexDigits[c >> 4]);
            out.push_back(HexDigits[c & 0x0F]);
        }
    }
    return out;
}

// For embedding a name in a JSON request body (core/drive.c json_escape also handles " \ and \n).
std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(c));
            out += escaped;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

// A Drive query string literal: backslash and quote are escaped with a backslash (core/drive.c).
std::string EscapeQueryLiteral(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 4);
    for (const char c : in) {
        if (c == '\'' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Folder names (core/syncstate.c syncstate_sanitize_name, core/syncjob.c cloud_folder_path)
// ---------------------------------------------------------------------------------------------

// Replaces every byte that cannot be part of a folder name with '_', cuts to outsz-1 bytes, drops
// trailing spaces and dots, and falls back to "sem-nome" (SwitchSaveSync's own fallback name, kept
// so both sides name the folder the same). Byte-wise on purpose, like the original.
std::string SanitizeName(const std::string& in, std::size_t outsz) {
    if (outsz == 0) {
        return std::string();
    }
    std::string out;
    for (std::size_t n = 0; n < in.size() && n + 1 < outsz; ++n) {
        const unsigned char c = static_cast<unsigned char>(in[n]);
        if (c == 0) {
            break;
        }
        const bool bad = c < 0x20 || std::strchr("/\\:*?\"<>|", static_cast<int>(c)) != nullptr;
        out.push_back(bad ? '_' : static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = "sem-nome";
        if (out.size() > outsz - 1) {
            out.resize(outsz - 1); // snprintf(out, outsz, ...) cuts to the buffer
        }
    }
    return out;
}

// "<game>/<account>", always two levels. The account part is what separates one save from another,
// so it is never the one that gets truncated.
std::string BuildCloudFolderPath(const std::string& game_name, const std::string& owner_name) {
    const std::string game = SanitizeName(game_name, GameNameBuffer);
    const std::string owner = SanitizeName(owner_name, OwnerNameBuffer);
    const std::size_t reserved = owner.size() + 2;
    const std::size_t room = FolderPathBuffer > reserved ? FolderPathBuffer - reserved : 0;
    return game.substr(0, std::min(game.size(), room)) + "/" + owner;
}

std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t stop = slash == std::string::npos ? path.size() : slash;
        if (stop > start) {
            parts.push_back(path.substr(start, stop - start));
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

// A name that came from the cloud becomes a path on the Xbox, so it must not be able to leave the
// save folder or name an NTFS stream.
void ValidateCloudName(const std::string& name) {
    bool bad = name.empty() || name == "." || name == ".." || name.back() == ' ' ||
               name.back() == '.';
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            bad = true;
        }
    }
    if (bad) {
        throw SyncError("a cloud file or folder has a name the Xbox cannot store: '" + name + "'");
    }
}

// ---------------------------------------------------------------------------------------------
// Local files
// ---------------------------------------------------------------------------------------------

bool ReadWholeFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Temp file then rename, so a power cut never leaves a half-written state or token file.
bool WriteWholeFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::vector<std::uint8_t> ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw SyncError("cannot read " + Utf8FromPath(path));
    }
    std::vector<std::uint8_t> bytes;
    std::vector<char> chunk(64 * 1024);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(chunk.data()),
                         reinterpret_cast<const std::uint8_t*>(chunk.data()) + got);
        }
    }
    if (in.bad()) {
        throw SyncError("cannot read " + Utf8FromPath(path));
    }
    return bytes;
}

void WriteFileBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw SyncError("cannot write " + Utf8FromPath(path));
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) {
        throw SyncError("cannot write " + Utf8FromPath(path));
    }
}

void CopyFileBytes(const fs::path& from, const fs::path& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
        throw SyncError("cannot read " + Utf8FromPath(from));
    }
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw SyncError("cannot write " + Utf8FromPath(to));
    }
    std::vector<char> chunk(64 * 1024);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            out.write(chunk.data(), got);
        }
    }
    if (in.bad()) {
        throw SyncError("cannot read " + Utf8FromPath(from));
    }
    out.flush();
    if (!out) {
        throw SyncError("cannot write " + Utf8FromPath(to));
    }
}

void RemoveTree(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

struct LocalEntry {
    std::string name; // file or folder name, UTF-8
    std::string rel;  // path below the scanned root, UTF-8, '/' separated
    fs::path full;
    bool is_dir = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

struct LocalTree {
    std::vector<LocalEntry> entries; // folders and files, a folder before what is inside it
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    std::int64_t newest = 0; // newest file time, 0 when there are no files
};

// One folder level, sorted by name. Anything that cannot be read is an error: a save that looks
// empty only because it could not be listed would be uploaded over a good cloud copy.
std::vector<LocalEntry> ScanLevel(const fs::path& dir, bool skip_meta,
                                  const std::string& rel_prefix) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        throw SyncError("cannot read folder " + Utf8FromPath(dir) + " (" + ec.message() + ")");
    }
    std::vector<LocalEntry> level;
    const fs::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        LocalEntry entry;
        entry.full = it->path();
        entry.name = Utf8FromPath(entry.full.filename());
        entry.rel = rel_prefix.empty() ? entry.name : rel_prefix + "/" + entry.name;
        std::error_code status_ec;
        const fs::file_status status = it->status(status_ec);
        if (status_ec) {
            throw SyncError("cannot read " + Utf8FromPath(entry.full) + " (" +
                            status_ec.message() + ")");
        }
        if (fs::is_directory(status)) {
            entry.is_dir = true;
        } else if (fs::is_regular_file(status)) {
            if (skip_meta && entry.name == SaveSizeFileName) {
                continue;
            }
            std::error_code size_ec;
            entry.size = static_cast<std::uint64_t>(it->file_size(size_ec));
            std::error_code time_ec;
            const fs::file_time_type stamp = it->last_write_time(time_ec);
            if (size_ec || time_ec) {
                throw SyncError("cannot read " + Utf8FromPath(entry.full));
            }
            entry.modified = UnixFromFileTime(stamp);
        } else {
            Log("skipping '" + entry.rel + "': not a regular file or folder");
            continue;
        }
        level.push_back(std::move(entry));
    }
    if (ec) {
        throw SyncError("cannot list folder " + Utf8FromPath(dir) + " (" + ec.message() + ")");
    }
    std::sort(level.begin(), level.end(),
              [](const LocalEntry& a, const LocalEntry& b) { return a.name < b.name; });
    return level;
}

void ScanInto(const fs::path& dir, bool is_root, bool skip_meta, const std::string& rel_prefix,
              LocalTree& tree) {
    const std::vector<LocalEntry> level = ScanLevel(dir, is_root && skip_meta, rel_prefix);
    for (const LocalEntry& entry : level) {
        tree.entries.push_back(entry);
        if (entry.is_dir) {
            ScanInto(entry.full, false, skip_meta, entry.rel, tree);
        } else {
            ++tree.files;
            tree.bytes += entry.size;
            tree.newest = std::max(tree.newest, entry.modified);
        }
    }
}

// A missing folder is an empty save (a game that never ran). skip_meta hides Eden's own file.
LocalTree ScanTree(const fs::path& root, bool skip_meta) {
    LocalTree tree;
    std::error_code ec;
    const bool present = fs::exists(root, ec);
    if (ec) {
        throw SyncError("cannot look at " + Utf8FromPath(root) + " (" + ec.message() + ")");
    }
    if (!present) {
        return tree;
    }
    if (!fs::is_directory(root, ec)) {
        throw SyncError(Utf8FromPath(root) + " is not a folder");
    }
    ScanInto(root, true, skip_meta, std::string(), tree);
    return tree;
}

// core/syncjob.c fingerprint_dir(), exactly: per file an FNV-1a over the NAME (not the path),
// mixed with size * 0x9E3779B97F4A7C15, then FNV-1a over every byte of the content; the per-file
// values are SUMMED (wrapping), so the order never matters. Folders add nothing. Time is not part
// of it, because a file downloaded from Drive is always "new".
std::uint64_t FingerprintTree(const LocalTree& tree) {
    constexpr std::uint64_t Basis = 1469598103934665603ull;
    constexpr std::uint64_t Prime = 1099511628211ull;
    constexpr std::uint64_t SizeMix = 0x9E3779B97F4A7C15ull;
    std::uint64_t sum = 0;
    std::vector<char> chunk(64 * 1024);
    for (const LocalEntry& entry : tree.entries) {
        if (entry.is_dir) {
            continue;
        }
        std::uint64_t h = Basis;
        for (const char ch : entry.name) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
            h *= Prime;
        }
        h ^= entry.size * SizeMix;
        std::ifstream in(entry.full, std::ios::binary);
        if (!in) {
            throw SyncError("cannot read " + Utf8FromPath(entry.full));
        }
        std::uint64_t seen = 0;
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = in.gcount();
            for (std::streamsize i = 0; i < got; ++i) {
                const auto value = static_cast<unsigned char>(chunk[static_cast<std::size_t>(i)]);
                h ^= static_cast<std::uint64_t>(value);
                h *= Prime;
            }
            seen += static_cast<std::uint64_t>(got);
        }
        if (in.bad() || seen != entry.size) {
            throw SyncError("cannot read " + Utf8FromPath(entry.full));
        }
        sum += h;
    }
    return sum;
}

void CopyTree(const fs::path& from, const fs::path& to, bool skip_meta) {
    const LocalTree tree = ScanTree(from, skip_meta);
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        throw SyncError("cannot create folder " + Utf8FromPath(to) + " (" + ec.message() + ")");
    }
    for (const LocalEntry& entry : tree.entries) {
        const fs::path destination = to / PathFromUtf8(entry.rel);
        if (entry.is_dir) {
            std::error_code dir_ec;
            fs::create_directories(destination, dir_ec);
            if (dir_ec) {
                throw SyncError("cannot create folder " + Utf8FromPath(destination) + " (" +
                                dir_ec.message() + ")");
            }
        } else {
            CopyFileBytes(entry.full, destination);
        }
    }
}

// Empties a folder but keeps the folder itself (and Eden's bookkeeping file when asked).
void ClearDirectory(const fs::path& dir, bool keep_meta) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return;
    }
    const std::vector<LocalEntry> level = ScanLevel(dir, keep_meta, std::string());
    for (const LocalEntry& entry : level) {
        std::error_code remove_ec;
        fs::remove_all(entry.full, remove_ec);
        if (remove_ec) {
            throw SyncError("cannot remove " + Utf8FromPath(entry.full) + " (" +
                            remove_ec.message() + ")");
        }
    }
}

// Removes a staging folder when the run ends, whatever way it ends.
class ScopedCleanup {
public:
    explicit ScopedCleanup(fs::path path) : path_(std::move(path)) {}
    ~ScopedCleanup() {
        RemoveTree(path_);
    }
    ScopedCleanup(const ScopedCleanup&) = delete;
    ScopedCleanup& operator=(const ScopedCleanup&) = delete;

private:
    fs::path path_;
};

// ---------------------------------------------------------------------------------------------
// The decision (core/syncjob.c syncjob_sync_title)
// ---------------------------------------------------------------------------------------------

enum class SyncAction { Nothing, Upload, Download, Equal, Conflict };

// Three numbers decide it: the Xbox save now, the cloud save now, and the save as it was when the
// two last matched (the marker). With only two numbers you can see they differ, not who moved.
SyncAction Decide(bool local_empty, bool cloud_empty, std::uint64_t local_fp,
                  std::uint64_t cloud_fp, bool has_marker, std::uint64_t marker) {
    if (local_empty && cloud_empty) {
        return SyncAction::Nothing;
    }
    if (cloud_empty) {
        return SyncAction::Upload; // first sync of this game
    }
    if (local_empty) {
        return SyncAction::Download; // nothing here to lose
    }
    if (local_fp == cloud_fp) {
        return SyncAction::Equal;
    }
    if (!has_marker) {
        return SyncAction::Conflict; // both sides have a save and no earlier sync says who moved
    }
    const bool local_changed = local_fp != marker;
    const bool cloud_changed = cloud_fp != marker;
    if (local_changed && cloud_changed) {
        return SyncAction::Conflict;
    }
    if (cloud_changed) {
        return SyncAction::Download;
    }
    return SyncAction::Upload;
}

// ---------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------

struct HttpRequest {
    std::string method; // GET, POST, PUT or PATCH
    std::string url;
    std::string bearer; // access token, empty for none
    std::string content_type;
    std::vector<std::uint8_t> body;
};

struct HttpReply {
    int status = 0; // 0 when no answer came back
    std::vector<std::uint8_t> body;
    std::string location; // the Location header, for resumable uploads
    std::string error;    // why there was no answer
    std::string Text() const {
        if (body.empty()) {
            return std::string();
        }
        return std::string(reinterpret_cast<const char*>(body.data()), body.size());
    }
};

std::vector<std::uint8_t> ToBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

class Transport {
public:
    virtual ~Transport() = default;
    virtual HttpReply Send(const HttpRequest& request) = 0;
};

wh::HttpMethod MethodFor(const std::string& name) {
    if (name == "POST") {
        return wh::HttpMethod::Post();
    }
    if (name == "PUT") {
        return wh::HttpMethod::Put();
    }
    if (name == "PATCH") {
        return wh::HttpMethod::Patch();
    }
    return wh::HttpMethod::Get();
}

// Windows.Web.Http, the same stack shader_share.cpp uses. One client per transport so the TLS
// connection to Google is reused between requests.
class WinrtTransport final : public Transport {
public:
    HttpReply Send(const HttpRequest& request) override {
        HttpReply reply;
        try {
            if (!client_) {
                client_.emplace();
            }
            const wf::Uri uri{winrt::to_hstring(request.url)};
            wh::HttpRequestMessage message{MethodFor(request.method), uri};
            if (!request.bearer.empty()) {
                message.Headers().Authorization(
                    whh::HttpCredentialsHeaderValue{L"Bearer", winrt::to_hstring(request.bearer)});
            }
            if (request.method == "GET") {
                message.Headers().TryAppendWithoutValidation(L"Cache-Control", L"no-cache");
            }
            if (!request.body.empty()) {
                wst::DataWriter writer;
                writer.WriteBytes(request.body);
                wh::HttpBufferContent content{writer.DetachBuffer()};
                if (!request.content_type.empty()) {
                    content.Headers().ContentType(whh::HttpMediaTypeHeaderValue::Parse(
                        winrt::to_hstring(request.content_type)));
                }
                message.Content(content);
            }
            const wh::HttpResponseMessage response = client_->SendRequestAsync(message).get();
            reply.status = static_cast<int>(response.StatusCode());
            try {
                const wf::Uri location = response.Headers().Location();
                if (location) {
                    reply.location = winrt::to_string(location.AbsoluteUri());
                }
            } catch (const winrt::hresult_error&) {
                // No readable Location header; only the start of a resumable upload needs one.
            }
            const wh::IHttpContent response_content = response.Content();
            if (response_content) {
                const wst::IBuffer buffer = response_content.ReadAsBufferAsync().get();
                const std::uint32_t length = buffer.Length();
                if (length != 0) {
                    reply.body.assign(buffer.data(), buffer.data() + length);
                }
            }
        } catch (const winrt::hresult_error& failure) {
            reply.status = 0;
            reply.body.clear();
            reply.error = winrt::to_string(failure.message());
        } catch (const std::exception& failure) {
            reply.status = 0;
            reply.body.clear();
            reply.error = failure.what();
        }
        return reply;
    }

private:
    std::optional<wh::HttpClient> client_;
};

// ---------------------------------------------------------------------------------------------
// JSON (Windows.Data.Json)
// ---------------------------------------------------------------------------------------------

bool ParseJsonObject(const std::string& text, wj::JsonObject& out) {
    std::string_view view(text);
    // Notepad and PowerShell like to write a byte order mark; the parser does not.
    if (view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF &&
        static_cast<unsigned char>(view[1]) == 0xBB &&
        static_cast<unsigned char>(view[2]) == 0xBF) {
        view.remove_prefix(3);
    }
    if (view.empty()) {
        return false;
    }
    try {
        return wj::JsonObject::TryParse(winrt::to_hstring(view), out);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// A string field, or "" when it is missing or not a string.
std::string JsonString(const wj::JsonObject& object, const wchar_t* key) {
    if (!object.HasKey(key)) {
        return std::string();
    }
    const wj::IJsonValue value = object.GetNamedValue(key);
    if (value.ValueType() != wj::JsonValueType::String) {
        return std::string();
    }
    return winrt::to_string(value.GetString());
}

double JsonNumber(const wj::JsonObject& object, const wchar_t* key, double fallback) {
    if (!object.HasKey(key)) {
        return fallback;
    }
    const wj::IJsonValue value = object.GetNamedValue(key);
    if (value.ValueType() != wj::JsonValueType::Number) {
        return fallback;
    }
    return value.GetNumber();
}

bool JsonArrayField(const wj::JsonObject& object, const wchar_t* key, wj::JsonArray& out) {
    if (!object.HasKey(key)) {
        return false;
    }
    const wj::IJsonValue value = object.GetNamedValue(key);
    if (value.ValueType() != wj::JsonValueType::Array) {
        return false;
    }
    out = value.GetArray();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Environment: everything that touches the outside world, so a run can be driven from a test
// ---------------------------------------------------------------------------------------------

struct ActiveUser {
    std::string dir_name; // 32 upper-case hex digits: the folder under user\save\0000000000000000
    std::string raw_uuid; // 32 lower-case hex digits: Eden's "future" save layout uses this one
    std::string name;     // the Eden profile name, UTF-8
};

struct Env {
    Transport* http = nullptr;
    fs::path local_state; // LocalState
    fs::path nand_dir;    // Eden's NAND directory
    std::function<bool(ActiveUser&)> resolve_user;
    std::function<void(std::chrono::milliseconds)> pause_for;
};

// The user Eden boots games as: Settings::values.current_user indexes the profile list, exactly as
// the ProfileManager constructor clamps it. Constructing a ProfileManager here creates the default
// "Player" profile when none exists yet, which is the same thing Eden's own boot would do.
bool ResolveEdenUser(ActiveUser& out) {
    try {
        const Service::Account::ProfileManager manager;
        const std::optional<Common::UUID> uuid = manager.GetLastOpenedUser();
        if (!uuid || uuid->IsInvalid()) {
            return false;
        }
        Service::Account::ProfileBase base{};
        if (!manager.GetProfileBase(*uuid, base)) {
            return false;
        }
        std::string profile_name;
        for (const auto name_byte : base.username) {
            if (name_byte == 0) {
                break;
            }
            profile_name.push_back(static_cast<char>(name_byte));
        }
        // savedata_factory.cpp GetFullPath(): {:016X}{:016X} of user_id[1], user_id[0].
        const u128 user_id = uuid->AsU128();
        out.dir_name = Hex16(user_id[1]) + Hex16(user_id[0]);
        out.raw_uuid = uuid->RawString();
        out.name = profile_name;
        return true;
    } catch (...) {
        return false;
    }
}

Env MakeEnv(Transport* http) {
    Env env;
    env.http = http;
    env.local_state = fs::path(std::wstring_view(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    env.nand_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    env.resolve_user = ResolveEdenUser;
    env.pause_for = [](std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
    };
    return env;
}

// ---------------------------------------------------------------------------------------------
// Credential and sign-in (core/oauth.c, core/credencial.c)
// ---------------------------------------------------------------------------------------------

struct Config {
    std::string client_id;
    std::string client_secret;
    std::string endpoint; // set instead of the two above when the auth server holds the credential
    std::string owner;    // optional account-folder name that replaces the Eden profile name
    bool backend = false;
};

// savesync.json. A complete client_id + client_secret wins over an endpoint, as in credencial.c.
bool LoadConfig(const Env& env, Config& out, std::string& reason) {
    std::string text;
    if (!ReadWholeFile(env.local_state / "savesync.json", text)) {
        reason = "LocalState\\savesync.json is missing";
        return false;
    }
    wj::JsonObject json;
    if (!ParseJsonObject(text, json)) {
        reason = "LocalState\\savesync.json is not valid JSON";
        return false;
    }
    Config config;
    config.client_id = Trim(JsonString(json, L"client_id"));
    config.client_secret = Trim(JsonString(json, L"client_secret"));
    config.owner = Trim(JsonString(json, L"owner"));
    std::string endpoint = Trim(JsonString(json, L"endpoint"));
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    if (!config.client_id.empty() && !config.client_secret.empty()) {
        out = config;
        return true;
    }
    const std::string lowered = LowerAscii(endpoint);
    if (!endpoint.empty() && lowered != "direct" && lowered != "direto" && lowered != "none") {
        if (lowered.rfind("https://", 0) != 0) {
            reason = "the endpoint in LocalState\\savesync.json must be an https URL";
            return false;
        }
        config.client_id.clear();
        config.client_secret.clear();
        config.endpoint = endpoint;
        config.backend = true;
        out = config;
        return true;
    }
    reason = "LocalState\\savesync.json needs client_id and client_secret";
    return false;
}

fs::path TokenPath(const Env& env) {
    return env.local_state / "savesync_token.json";
}

std::string ReadRefreshToken(const Env& env) {
    std::string text;
    if (!ReadWholeFile(TokenPath(env), text)) {
        return std::string();
    }
    wj::JsonObject json;
    if (!ParseJsonObject(text, json)) {
        return std::string();
    }
    return JsonString(json, L"refresh_token");
}

bool WriteRefreshToken(const Env& env, const std::string& token) {
    wj::JsonObject json;
    json.SetNamedValue(L"refresh_token",
                       wj::JsonValue::CreateStringValue(winrt::to_hstring(token)));
    return WriteWholeFile(TokenPath(env), winrt::to_string(json.Stringify()));
}

void RemoveRefreshToken(const Env& env) {
    std::error_code ec;
    fs::remove(TokenPath(env), ec);
}

using FormFields = std::vector<std::pair<std::string, std::string>>;

HttpReply PostForm(const Env& env, const std::string& url, const FormFields& fields) {
    std::string body;
    for (const auto& field : fields) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += UrlEncode(field.first) + "=" + UrlEncode(field.second);
    }
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.content_type = "application/x-www-form-urlencoded";
    request.body = ToBytes(body);
    return env.http->Send(request);
}

// The sign-in that is waiting for the user, between StartDeviceLogin and PollDeviceLogin.
struct LoginState {
    std::mutex mutex;
    bool active = false;
    std::string device_code;
    int interval_seconds = 5;
    std::chrono::steady_clock::time_point deadline{};
};

LoginState g_login;
std::atomic<bool> g_login_cancel{false};
std::mutex g_sync_mutex;

void EndLogin() {
    const std::lock_guard<std::mutex> guard(g_login.mutex);
    g_login.active = false;
    g_login.device_code.clear();
}

DeviceLogin StartLogin(const Env& env) {
    DeviceLogin login;
    Config config;
    std::string reason;
    if (!LoadConfig(env, config, reason)) {
        login.error = reason;
        return login;
    }
    g_login_cancel.store(false);
    const HttpReply reply =
        config.backend
            ? PostForm(env, config.endpoint + "/device", {{"scope", DriveScope}})
            : PostForm(env, DeviceCodeUrl,
                       {{"client_id", config.client_id}, {"scope", DriveScope}});
    if (reply.status == 0) {
        login.error = "cannot reach Google (" + reply.error + ")";
        return login;
    }
    if (reply.status == 429) {
        login.error = "the sign-in server is limiting this address, try again in a few minutes";
        return login;
    }
    if (reply.status != 200) {
        login.error = "Google answered HTTP " + std::to_string(reply.status) +
                      " to the device code request";
        return login;
    }
    wj::JsonObject json;
    if (!ParseJsonObject(reply.Text(), json)) {
        login.error = "Google sent something that is not JSON";
        return login;
    }
    const std::string device_code = JsonString(json, L"device_code");
    const std::string user_code = JsonString(json, L"user_code");
    // The field is "verification_url" or "verification_uri" depending on the API version.
    std::string url = JsonString(json, L"verification_url");
    if (url.empty()) {
        url = JsonString(json, L"verification_uri");
    }
    if (url.empty()) {
        url = "https://www.google.com/device";
    }
    if (device_code.empty() || user_code.empty()) {
        login.error = "Google's answer had no device_code or user_code (check the client_id)";
        return login;
    }
    const int expires = static_cast<int>(JsonNumber(json, L"expires_in", 1800));
    const int interval = static_cast<int>(JsonNumber(json, L"interval", 5));
    {
        const std::lock_guard<std::mutex> guard(g_login.mutex);
        g_login.active = true;
        g_login.device_code = device_code;
        g_login.interval_seconds = std::clamp(interval, 1, 60);
        g_login.deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(std::max(expires, 1));
    }
    login.ok = true;
    login.user_code = user_code;
    login.verification_url = url;
    login.url_with_code = url + "?user_code=" + user_code;
    login.expires_in_seconds = std::max(expires, 1);
    Log("sign-in started, the code is valid for " + std::to_string(login.expires_in_seconds) +
        " s");
    return login;
}

LoginResult PollLogin(const Env& env) {
    std::string device_code;
    int interval = 5;
    std::chrono::steady_clock::time_point deadline{};
    {
        const std::lock_guard<std::mutex> guard(g_login.mutex);
        if (!g_login.active) {
            Log("sign-in poll without a sign-in in progress");
            return LoginResult::Failed;
        }
        device_code = g_login.device_code;
        interval = g_login.interval_seconds;
        deadline = g_login.deadline;
    }
    Config config;
    std::string reason;
    if (!LoadConfig(env, config, reason)) {
        Log("sign-in poll: " + reason);
        EndLogin();
        return LoginResult::Failed;
    }

    LoginResult outcome = LoginResult::Expired;
    while (std::chrono::steady_clock::now() < deadline) {
        // Wait one interval in short slices so a cancel is noticed quickly.
        for (int waited = 0; waited < interval * 1000 && !g_login_cancel.load(); waited += 100) {
            env.pause_for(std::chrono::milliseconds(100));
        }
        if (g_login_cancel.load()) {
            outcome = LoginResult::Cancelled;
            break;
        }
        const HttpReply reply =
            config.backend
                ? PostForm(env, config.endpoint + "/token", {{"device_code", device_code}})
                : PostForm(env, TokenUrl,
                           {{"client_id", config.client_id},
                            {"client_secret", config.client_secret},
                            {"device_code", device_code},
                            {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
        if (reply.status == 0) {
            continue; // a Wi-Fi hiccup, ask again at the next tick
        }
        if (reply.status == 429) {
            interval = std::min(interval + 10, 60); // the server asked us to ease off
            continue;
        }
        wj::JsonObject json;
        const bool parsed = ParseJsonObject(reply.Text(), json);
        if (reply.status == 200) {
            const std::string refresh_token =
                parsed ? JsonString(json, L"refresh_token") : std::string();
            if (refresh_token.empty()) {
                // Google skips the refresh token when the app was authorized before. The owner has
                // to revoke the old grant at myaccount.google.com/permissions.
                Log("sign-in: Google confirmed but sent no refresh token");
                outcome = LoginResult::Failed;
            } else if (!WriteRefreshToken(env, refresh_token)) {
                Log("sign-in: could not store the refresh token");
                outcome = LoginResult::Failed;
            } else {
                Log("sign-in finished, the refresh token is stored");
                outcome = LoginResult::SignedIn;
            }
            break;
        }
        const std::string code = parsed ? JsonString(json, L"error") : std::string();
        if (code == "authorization_pending") {
            continue;
        }
        if (code == "slow_down") {
            interval = std::min(interval + 5, 60);
            continue;
        }
        if (code == "access_denied") {
            outcome = LoginResult::Denied;
            break;
        }
        if (code == "expired_token" || code == "invalid_grant") {
            outcome = LoginResult::Expired;
            break;
        }
        // invalid_client, invalid_request, unauthorized_client...: retrying cannot help.
        Log("sign-in: Google answered HTTP " + std::to_string(reply.status) + " " + code);
        outcome = LoginResult::Failed;
        break;
    }
    EndLogin();
    return outcome;
}

enum class RefreshOutcome { Ok, Revoked, RateLimited, Failed };

// core/oauth.c oauth_refresh_access_token(): an invalid_grant answer means the refresh token is
// dead (account disconnected, password changed, keys reset) and will never work again.
RefreshOutcome RefreshAccessToken(const Env& env, const Config& config,
                                  const std::string& refresh_token, std::string& access_token,
                                  std::string& detail) {
    const HttpReply reply =
        config.backend
            ? PostForm(env, config.endpoint + "/refresh", {{"refresh_token", refresh_token}})
            : PostForm(env, TokenUrl,
                       {{"client_id", config.client_id},
                        {"client_secret", config.client_secret},
                        {"refresh_token", refresh_token},
                        {"grant_type", "refresh_token"}});
    if (reply.status == 0) {
        detail = "no answer from Google (" + reply.error + ")";
        return RefreshOutcome::Failed;
    }
    if (reply.status == 429) {
        detail = "the sign-in server is rate limiting this address";
        return RefreshOutcome::RateLimited;
    }
    const std::string text = reply.Text();
    if (reply.status == 400 && text.find("invalid_grant") != std::string::npos) {
        return RefreshOutcome::Revoked;
    }
    if (reply.status != 200) {
        detail = "Google answered HTTP " + std::to_string(reply.status) + " to the token refresh";
        return RefreshOutcome::Failed;
    }
    wj::JsonObject json;
    if (!ParseJsonObject(text, json)) {
        detail = "Google's token answer was not JSON";
        return RefreshOutcome::Failed;
    }
    access_token = JsonString(json, L"access_token");
    if (access_token.empty()) {
        detail = "Google's token answer had no access_token";
        return RefreshOutcome::Failed;
    }
    return RefreshOutcome::Ok;
}

// ---------------------------------------------------------------------------------------------
// Google Drive (core/drive.c)
// ---------------------------------------------------------------------------------------------

struct CloudItem {
    std::string id;
    std::string name;
    std::string mime;
    bool is_folder = false;
    bool has_size = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

// The parts of multipart/related that Drive wants (core/http.c build_multipart_related_body): a
// JSON part with the metadata, then the bytes, no Content-Disposition.
std::vector<std::uint8_t> BuildMultipartRelated(const std::string& boundary,
                                                const std::string& metadata_json,
                                                const std::string& mime,
                                                const std::vector<std::uint8_t>& data) {
    const std::string head = "--" + boundary +
                             "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                             metadata_json + "\r\n--" + boundary + "\r\nContent-Type: " + mime +
                             "\r\n\r\n";
    const std::string tail = "\r\n--" + boundary + "--";
    std::vector<std::uint8_t> body;
    body.reserve(head.size() + data.size() + tail.size());
    body.insert(body.end(), head.begin(), head.end());
    body.insert(body.end(), data.begin(), data.end());
    body.insert(body.end(), tail.begin(), tail.end());
    return body;
}

class Drive {
public:
    Drive(const Env& env, std::string access_token) : env_(env), token_(std::move(access_token)) {}

    // find_by_name() with the folder mime type. A failed lookup is an error, never "not found":
    // creating after a failed search would make a second folder with the same name.
    std::optional<std::string> FindFolder(const std::string& parent_id, const std::string& name) {
        std::string query = "name='" + EscapeQueryLiteral(name) + "' and trashed=false";
        if (!parent_id.empty()) {
            query += " and '" + EscapeQueryLiteral(parent_id) + "' in parents";
        }
        query += std::string(" and mimeType='") + FolderMime + "'";
        HttpRequest request;
        request.method = "GET";
        request.url = std::string(FilesUrl) + "?q=" + UrlEncode(query) +
                      "&fields=" + UrlEncode("files(id,name)") + "&pageSize=1";
        const HttpReply reply = Call(std::move(request), true, "find folder");
        if (reply.status != 200) {
            Fail("find folder '" + name + "'", reply);
        }
        wj::JsonObject json;
        if (!ParseJsonObject(reply.Text(), json)) {
            throw SyncError("find folder '" + name + "': Google's answer was not JSON");
        }
        // core/drive.c find_by_name(): an answer without a "files" key matched nothing.
        if (!json.HasKey(L"files")) {
            return std::nullopt;
        }
        wj::JsonArray files;
        if (!JsonArrayField(json, L"files", files)) {
            throw SyncError("find folder '" + name + "': Google's file list was malformed");
        }
        if (files.Size() == 0) {
            return std::nullopt;
        }
        const std::string id = JsonString(files.GetObjectAt(0), L"id");
        if (id.empty()) {
            return std::nullopt;
        }
        return id;
    }

    std::string CreateFolder(const std::string& parent_id, const std::string& name) {
        std::string body =
            "{\"name\":\"" + JsonEscape(name) + "\",\"mimeType\":\"" + FolderMime + "\"";
        if (!parent_id.empty()) {
            body += ",\"parents\":[\"" + JsonEscape(parent_id) + "\"]";
        }
        body += "}";
        HttpRequest request;
        request.method = "POST";
        request.url = FilesUrl;
        request.content_type = "application/json; charset=UTF-8";
        request.body = ToBytes(body);
        // Not retried on a lost answer: the folder may exist already, and a second one would split
        // the save between two folders.
        const HttpReply reply = Call(std::move(request), false, "create folder");
        if (reply.status != 200 && reply.status != 201) {
            Fail("create folder '" + name + "'", reply);
        }
        wj::JsonObject json;
        if (!ParseJsonObject(reply.Text(), json)) {
            throw SyncError("create folder '" + name + "': Google's answer was not JSON");
        }
        const std::string id = JsonString(json, L"id");
        if (id.empty()) {
            throw SyncError("create folder '" + name + "': Google's answer had no id");
        }
        return id;
    }

    std::string EnsureFolder(const std::string& parent_id, const std::string& name) {
        const std::optional<std::string> found = FindFolder(parent_id, name);
        if (found) {
            return *found;
        }
        return CreateFolder(parent_id, name);
    }

    // Every child of a folder that is not in the trash, all pages (SwitchSaveSync stops at 1000).
    std::vector<CloudItem> ListChildren(const std::string& folder_id) {
        std::vector<CloudItem> items;
        std::string page_token;
        for (;;) {
            const std::string query =
                "'" + EscapeQueryLiteral(folder_id) + "' in parents and trashed=false";
            HttpRequest request;
            request.method = "GET";
            request.url = std::string(FilesUrl) + "?q=" + UrlEncode(query) + "&fields=" +
                          UrlEncode("nextPageToken,files(id,name,mimeType,modifiedTime,size)") +
                          "&pageSize=1000";
            if (!page_token.empty()) {
                request.url += "&pageToken=" + UrlEncode(page_token);
            }
            const HttpReply reply = Call(std::move(request), true, "list folder");
            if (reply.status != 200) {
                Fail("list folder", reply);
            }
            wj::JsonObject json;
            wj::JsonArray files;
            // 200 without a "files" array is Google answering with something else, not an empty
            // folder.
            if (!ParseJsonObject(reply.Text(), json) || !JsonArrayField(json, L"files", files)) {
                throw SyncError("list folder: Google's answer had no file list");
            }
            for (std::uint32_t i = 0; i < files.Size(); ++i) {
                const wj::JsonObject entry = files.GetObjectAt(i);
                CloudItem item;
                item.id = JsonString(entry, L"id");
                item.name = JsonString(entry, L"name");
                item.mime = JsonString(entry, L"mimeType");
                item.is_folder = item.mime == FolderMime;
                const std::string size_text = JsonString(entry, L"size");
                if (!size_text.empty()) {
                    item.has_size = true;
                    item.size =
                        static_cast<std::uint64_t>(std::strtoull(size_text.c_str(), nullptr, 10));
                }
                item.modified = ParseRfc3339(JsonString(entry, L"modifiedTime"));
                if (!item.id.empty()) {
                    items.push_back(std::move(item));
                }
            }
            page_token = JsonString(json, L"nextPageToken");
            if (page_token.empty()) {
                break;
            }
        }
        return items;
    }

    void Download(const std::string& file_id, const fs::path& destination, bool has_size,
                  std::uint64_t expected_size, const std::string& label) {
        HttpRequest request;
        request.method = "GET";
        request.url = std::string(FilesUrl) + "/" + UrlEncode(file_id) + "?alt=media";
        const HttpReply reply = Call(std::move(request), true, "download");
        // Any 2xx counts, like http_download_to_file() (an empty file may come back as 204).
        if (reply.status < 200 || reply.status >= 300) {
            Fail("download '" + label + "'", reply);
        }
        if (reply.body.size() > MaxFileBytes) {
            throw SyncError("download '" + label + "': the file is too large");
        }
        // The listing said how long the file is; a shorter body is a cut connection, and writing it
        // would put a truncated save file on the console.
        if (has_size && reply.body.size() != expected_size) {
            throw SyncError("download '" + label + "': the transfer was cut short");
        }
        WriteFileBytes(destination, reply.body);
    }

    // drive_upload(): update the file with this name when there is one (existing_id), else create
    // it.
    void Upload(const std::string& folder_id, const std::string& name, const fs::path& source,
                std::uint64_t size, const std::string& existing_id) {
        if (size > MaxFileBytes) {
            throw SyncError("upload '" + name + "': the file is too large");
        }
        const std::vector<std::uint8_t> data = ReadFileBytes(source);
        if (data.size() != size) {
            throw SyncError("upload '" + name + "': the file changed while it was being read");
        }
        const bool updating = !existing_id.empty();
        std::string metadata = "{\"name\":\"" + JsonEscape(name) + "\"";
        if (!updating) {
            metadata += ",\"parents\":[\"" + JsonEscape(folder_id) + "\"]";
        }
        metadata += "}";
        std::string target = UploadUrl;
        if (updating) {
            target += "/" + UrlEncode(existing_id);
        }
        if (size <= MultipartMax) {
            HttpRequest request;
            request.method = updating ? "PATCH" : "POST";
            request.url = target + "?uploadType=multipart";
            request.content_type = std::string("multipart/related; boundary=") + MultipartBoundary;
            request.body = BuildMultipartRelated(MultipartBoundary, metadata, FileMime, data);
            // A create whose answer got lost might have succeeded; do not repeat it blindly.
            const HttpReply reply = Call(std::move(request), updating, "upload");
            if (reply.status != 200 && reply.status != 201) {
                Fail("upload '" + name + "'", reply);
            }
            return;
        }
        // Resumable: the first request carries only the metadata and returns the session URI, the
        // second one sends the bytes. The file does not exist until the second one completes, so
        // repeating the pair is safe.
        for (int attempt = 1; attempt <= 2; ++attempt) {
            HttpRequest open_request;
            open_request.method = updating ? "PATCH" : "POST";
            open_request.url = target + "?uploadType=resumable";
            open_request.content_type = "application/json; charset=UTF-8";
            open_request.body = ToBytes(metadata);
            const HttpReply opened = Call(std::move(open_request), true, "start upload");
            if (opened.status != 200 || opened.location.rfind("https://", 0) != 0) {
                Fail("start upload '" + name + "'", opened);
            }
            HttpRequest put_request;
            put_request.method = "PUT";
            put_request.url = opened.location;
            put_request.content_type = FileMime;
            put_request.body = data;
            const HttpReply sent = Call(std::move(put_request), true, "upload");
            if (sent.status == 200 || sent.status == 201) {
                return;
            }
            if (attempt == 2 || sent.status == 401 || sent.status == 403 || sent.status == 404) {
                Fail("upload '" + name + "'", sent);
            }
            Log("upload '" + name + "' did not complete, starting it again");
        }
    }

    // A PATCH {"trashed":true}, not a DELETE: the file stays recoverable for 30 days. 404 counts as
    // done, because "no longer in the folder" is all that is wanted.
    bool Trash(const std::string& file_id) {
        HttpRequest request;
        request.method = "PATCH";
        request.url = std::string(FilesUrl) + "/" + UrlEncode(file_id);
        request.content_type = "application/json; charset=UTF-8";
        request.body = ToBytes("{\"trashed\":true}");
        const HttpReply reply = Call(std::move(request), true, "trash");
        return reply.status == 200 || reply.status == 204 || reply.status == 404;
    }

private:
    // Rate limits and server hiccups are retried with a growing pause. A request that is not safe
    // to repeat (a create) is only retried when the server said it did not process it.
    HttpReply Call(HttpRequest request, bool idempotent, const std::string& what) {
        request.bearer = token_;
        constexpr int MaxAttempts = 4;
        HttpReply reply;
        for (int attempt = 1; attempt <= MaxAttempts; ++attempt) {
            reply = env_.http->Send(request);
            const bool refused = reply.status == 429 || reply.status == 503 ||
                                 (reply.status == 403 &&
                                  reply.Text().find("ateLimitExceeded") != std::string::npos);
            const bool hiccup = reply.status == 0 || reply.status == 500 || reply.status == 502 ||
                                reply.status == 504;
            const bool retry = refused || (hiccup && idempotent);
            if (!retry || attempt == MaxAttempts) {
                break;
            }
            Log(what + ": HTTP " + std::to_string(reply.status) + ", trying again");
            env_.pause_for(std::chrono::milliseconds(500 << attempt));
        }
        return reply;
    }

    [[noreturn]] static void Fail(const std::string& what, const HttpReply& reply) {
        if (reply.status == 0) {
            throw SyncError(what + ": no answer from Google (" + reply.error + ")");
        }
        if (reply.status == 401) {
            throw SyncError(what + ": Google rejected the access token (HTTP 401)");
        }
        throw SyncError(what + ": Google answered HTTP " + std::to_string(reply.status));
    }

    const Env& env_;
    std::string token_;
};

// ---------------------------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------------------------

class Reporter {
public:
    explicit Reporter(const ProgressCallback& callback) : callback_(callback) {}

    void Emit(SyncStage stage, std::uint32_t done, std::uint32_t total,
              const std::string& item) const {
        if (!callback_) {
            return;
        }
        SyncProgress update;
        update.stage = stage;
        update.done = done;
        update.total = total;
        update.item = item;
        try {
            callback_(update);
        } catch (...) {
            // A broken progress bar must not abort a save transfer.
        }
    }

private:
    const ProgressCallback& callback_;
};

// ---------------------------------------------------------------------------------------------
// State (the role core/syncstate.c plays with pastas.txt and the rev-*.txt markers)
// ---------------------------------------------------------------------------------------------

struct SavedState {
    bool has_fingerprint = false;
    std::uint64_t fingerprint = 0;
    std::string folder; // "<game>/<account>" used the last time
};

fs::path StatePath(const Env& env) {
    return env.local_state / "savesync_state.json";
}

// A broken or missing file only means "no marker yet", which makes the next sync ask instead of
// guess, so reading it can never be the thing that overwrites a save.
SavedState LoadState(const Env& env, const std::string& key) {
    SavedState state;
    std::string text;
    if (!ReadWholeFile(StatePath(env), text)) {
        return state;
    }
    wj::JsonObject root;
    if (!ParseJsonObject(text, root)) {
        Log("savesync_state.json is unreadable; no markers are used");
        return state;
    }
    if (!root.HasKey(L"saves") ||
        root.GetNamedValue(L"saves").ValueType() != wj::JsonValueType::Object) {
        return state;
    }
    const wj::JsonObject saves = root.GetNamedObject(L"saves");
    const winrt::hstring wide_key = winrt::to_hstring(key);
    if (!saves.HasKey(wide_key) ||
        saves.GetNamedValue(wide_key).ValueType() != wj::JsonValueType::Object) {
        return state;
    }
    const wj::JsonObject entry = saves.GetNamedObject(wide_key);
    state.has_fingerprint = ParseHex64(JsonString(entry, L"fingerprint"), state.fingerprint);
    state.folder = JsonString(entry, L"folder");
    return state;
}

// Records the folder used and, when the two sides are known to match, their fingerprint. Without a
// fingerprint the one already stored is kept. Never throws: a failed write is only logged.
void StoreState(const Env& env, const std::string& key, std::optional<std::uint64_t> fingerprint,
                const std::string& folder) {
    try {
        wj::JsonObject root;
        std::string text;
        if (ReadWholeFile(StatePath(env), text)) {
            wj::JsonObject parsed;
            if (ParseJsonObject(text, parsed)) {
                root = parsed;
            }
        }
        wj::JsonObject saves;
        if (root.HasKey(L"saves") &&
            root.GetNamedValue(L"saves").ValueType() == wj::JsonValueType::Object) {
            saves = root.GetNamedObject(L"saves");
        }
        const SavedState previous = LoadState(env, key);
        wj::JsonObject entry;
        std::optional<std::uint64_t> kept = fingerprint;
        if (!kept && previous.has_fingerprint) {
            kept = previous.fingerprint;
        }
        if (kept) {
            entry.SetNamedValue(L"fingerprint",
                                wj::JsonValue::CreateStringValue(winrt::to_hstring(Hex16(*kept))));
        }
        entry.SetNamedValue(L"folder",
                            wj::JsonValue::CreateStringValue(winrt::to_hstring(folder)));
        entry.SetNamedValue(L"synced_at",
                            wj::JsonValue::CreateNumberValue(static_cast<double>(NowUnix())));
        saves.SetNamedValue(winrt::to_hstring(key), entry);
        root.SetNamedValue(L"version", wj::JsonValue::CreateNumberValue(1));
        root.SetNamedValue(L"saves", saves);
        if (!WriteWholeFile(StatePath(env), winrt::to_string(root.Stringify()))) {
            Log("could not write savesync_state.json");
        }
    } catch (const winrt::hresult_error& failure) {
        Log("could not update savesync_state.json: " + winrt::to_string(failure.message()));
    } catch (const std::exception& failure) {
        Log(std::string("could not update savesync_state.json: ") + failure.what());
    }
}

// ---------------------------------------------------------------------------------------------
// One save
// ---------------------------------------------------------------------------------------------

struct Target {
    std::uint64_t title_id = 0;
    std::string title_hex;
    ActiveUser user;
    std::string key;   // state key: "<TITLEID>/<user folder>"
    std::string owner; // the account-folder name used in the cloud
    fs::path local_dir;
    fs::path staging_dir;
    fs::path backup_dir;
};

// savedata_factory.cpp GetFullPath(): <nand>/user/save/0000000000000000/<user>/<TITLEID>, unless
// the "future" location <nand>/user/save/account/<uuid>/<TITLEID & ~0xFF>/0 exists, which Eden
// prefers.
fs::path SaveDirFor(const Env& env, std::uint64_t title_id, const ActiveUser& user) {
    const fs::path future = env.nand_dir / "user" / "save" / "account" / user.raw_uuid /
                            Hex16(title_id & ~static_cast<std::uint64_t>(0xFF)) / "0";
    std::error_code ec;
    if (fs::exists(future, ec) && !ec) {
        return future;
    }
    return env.nand_dir / "user" / "save" / "0000000000000000" / user.dir_name / Hex16(title_id);
}

Target BuildTarget(const Env& env, std::uint64_t title_id, const ActiveUser& user,
                   const Config& config) {
    Target target;
    target.title_id = title_id;
    target.title_hex = Hex16(title_id);
    target.user = user;
    target.key = target.title_hex + "/" + user.dir_name;
    target.owner = config.owner.empty() ? user.name : config.owner;
    target.local_dir = SaveDirFor(env, title_id, user);
    const std::string scratch = target.title_hex + "_" + user.dir_name;
    target.staging_dir = env.local_state / "savesync_staging" / scratch;
    target.backup_dir = env.local_state / "savesync_backup" / scratch;
    return target;
}

// Does any OTHER Eden profile (or the device-save folder) hold a save of this title? The cloud
// folder is found by the account name, and when that name is not there the single account folder
// that exists is used (core/syncjob.c cloud_title_folder: the rescue for a renamed profile). That
// guess is only honest while this is the only save of the game on this console; with two the name
// is the identity, and guessing would hand one person's save to the other.
bool OtherSavesExist(const Env& env, const Target& target) {
    const fs::path users_dir = env.nand_dir / "user" / "save" / "0000000000000000";
    std::error_code ec;
    if (!fs::exists(users_dir, ec)) {
        return static_cast<bool>(ec);
    }
    fs::directory_iterator it(users_dir, ec);
    if (ec) {
        return true;
    }
    const fs::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        const std::string folder_name = Utf8FromPath(it->path().filename());
        if (EqualsIgnoreCase(folder_name, target.user.dir_name)) {
            continue;
        }
        try {
            if (ScanTree(it->path() / target.title_hex, false).files > 0) {
                return true;
            }
        } catch (const SyncError&) {
            return true;
        }
    }
    return static_cast<bool>(ec);
}

struct CloudLocation {
    bool found = false;
    std::string folder_id; // the folder that holds the save files
    std::string path;      // "<game>/<account>" as it is named in the cloud
};

std::optional<std::string> FindPath(Drive& drive, const std::string& root_id,
                                    const std::string& path) {
    const std::vector<std::string> parts = SplitPath(path);
    if (parts.empty()) {
        return std::nullopt;
    }
    std::string parent = root_id;
    for (const std::string& part : parts) {
        const std::optional<std::string> next = drive.FindFolder(parent, part);
        if (!next) {
            return std::nullopt;
        }
        parent = *next;
    }
    return parent;
}

std::string EnsureFolderPath(Drive& drive, const std::string& root_id, const std::string& path) {
    std::string parent = root_id;
    for (const std::string& part : SplitPath(path)) {
        parent = drive.EnsureFolder(parent, part);
    }
    return parent;
}

// Finds where this save lives in the cloud. Reading never creates anything: a freshly created
// empty folder would pass for "there is no backup" and invite an upload over a good save.
//   1. the folder used the last time (survives a renamed profile; only a two-level path counts);
//   2. the folder the name computes to;
//   3. if the game folder holds exactly ONE account folder and this is the only local save, that
//      one.
CloudLocation LocateCloudFolder(Drive& drive, const std::string& root_id,
                                const std::string& recalled, const std::string& computed,
                                const std::function<bool()>& rescue_allowed) {
    CloudLocation location;
    std::vector<std::string> candidates;
    if (recalled.find('/') != std::string::npos) {
        candidates.push_back(recalled);
    }
    if (!computed.empty() && computed != recalled) {
        candidates.push_back(computed);
    }
    for (const std::string& candidate : candidates) {
        const std::optional<std::string> id = FindPath(drive, root_id, candidate);
        if (id) {
            location.found = true;
            location.folder_id = *id;
            location.path = candidate;
            return location;
        }
    }
    if (computed.empty() || !rescue_allowed()) {
        return location;
    }
    const std::vector<std::string> parts = SplitPath(computed);
    if (parts.size() != 2) {
        return location;
    }
    const std::optional<std::string> game_id = drive.FindFolder(root_id, parts[0]);
    if (!game_id) {
        return location;
    }
    std::vector<CloudItem> account_folders;
    for (const CloudItem& child : drive.ListChildren(*game_id)) {
        if (child.is_folder) {
            account_folders.push_back(child);
        }
    }
    if (account_folders.size() == 1) {
        location.found = true;
        location.folder_id = account_folders[0].id;
        location.path = parts[0] + "/" + account_folders[0].name;
        Log("no cloud folder for this account name; using the only one in the game folder: '" +
            location.path + "'");
    }
    return location;
}

// A save that is "not there yet" is most often a game name that differs from the Switch's. Listing
// what is in the cloud lets the diagnostic file show the mismatch. Never fails a sync.
void LogGameFolders(Drive& drive, const std::string& root_id, const std::string& wanted) {
    try {
        std::string names;
        std::size_t shown = 0;
        for (const CloudItem& child : drive.ListChildren(root_id)) {
            if (!child.is_folder) {
                continue;
            }
            if (shown++ < 40) {
                names += (names.empty() ? "'" : ", '") + child.name + "'";
            }
        }
        Log("no cloud folder '" + wanted + "'; game folders in the cloud: " +
            (names.empty() ? std::string("none") : names));
    } catch (const std::exception&) {
        // The listing is only a courtesy.
    }
}

struct CloudFile {
    std::string id;
    std::string rel;
    bool has_size = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

struct CloudSnapshot {
    std::vector<std::string> dirs;
    std::vector<CloudFile> files;
    std::uint64_t bytes = 0;
    std::int64_t newest = 0;
};

void ListTreeInto(Drive& drive, const std::string& folder_id, const std::string& prefix,
                  CloudSnapshot& snapshot) {
    const std::vector<CloudItem> items = drive.ListChildren(folder_id);
    for (const CloudItem& item : items) {
        ValidateCloudName(item.name);
        const std::string rel = prefix.empty() ? item.name : prefix + "/" + item.name;
        if (item.is_folder) {
            snapshot.dirs.push_back(rel);
            ListTreeInto(drive, item.id, rel, snapshot);
        } else if (item.mime.rfind("application/vnd.google-apps.", 0) == 0) {
            // A Google Doc cannot be downloaded as bytes, and a game save is never one.
            Log("skipping '" + rel + "': a Google document, not a save file");
        } else if (prefix.empty() && item.name == SaveSizeFileName) {
            // Eden's own bookkeeping file must never overwrite the one next to the local save. (The
            // next upload moves it to the trash, since the Xbox save does not list it.)
            Log("ignoring '" + rel + "' in the cloud: it is Eden's own file, not part of a save");
        } else {
            CloudFile file;
            file.id = item.id;
            file.rel = rel;
            file.has_size = item.has_size;
            file.size = item.size;
            file.modified = item.modified;
            snapshot.files.push_back(std::move(file));
        }
    }
}

// The whole cloud folder as a flat list. When two files share a path (an old SwitchSaveSync bug
// could leave duplicates) the newer one is the one that counts.
CloudSnapshot ListCloudTree(Drive& drive, const std::string& folder_id) {
    CloudSnapshot snapshot;
    ListTreeInto(drive, folder_id, std::string(), snapshot);
    std::sort(snapshot.dirs.begin(), snapshot.dirs.end());
    snapshot.dirs.erase(std::unique(snapshot.dirs.begin(), snapshot.dirs.end()),
                        snapshot.dirs.end());
    std::map<std::string, std::size_t> chosen;
    std::vector<CloudFile> kept;
    for (const CloudFile& file : snapshot.files) {
        const auto found = chosen.find(file.rel);
        if (found == chosen.end()) {
            chosen.emplace(file.rel, kept.size());
            kept.push_back(file);
            continue;
        }
        Log("two cloud files share the path '" + file.rel + "'; the newer one is used");
        CloudFile& existing = kept[found->second];
        if (file.modified > existing.modified) {
            existing = file;
        }
    }
    snapshot.files = std::move(kept);
    for (const CloudFile& file : snapshot.files) {
        snapshot.bytes += file.size;
        snapshot.newest = std::max(snapshot.newest, file.modified);
    }
    return snapshot;
}

void DownloadSnapshot(Drive& drive, const CloudSnapshot& snapshot, const fs::path& staging,
                      const Reporter& report) {
    RemoveTree(staging);
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (ec) {
        throw SyncError("cannot create the staging folder (" + ec.message() + ")");
    }
    for (const std::string& dir : snapshot.dirs) {
        std::error_code dir_ec;
        fs::create_directories(staging / PathFromUtf8(dir), dir_ec);
        if (dir_ec) {
            throw SyncError("cannot create folder '" + dir + "' (" + dir_ec.message() + ")");
        }
    }
    const std::uint32_t total = static_cast<std::uint32_t>(snapshot.files.size());
    std::uint32_t done = 0;
    report.Emit(SyncStage::Downloading, 0, total, std::string());
    for (const CloudFile& file : snapshot.files) {
        drive.Download(file.id, staging / PathFromUtf8(file.rel), file.has_size, file.size,
                       file.rel);
        ++done;
        report.Emit(SyncStage::Downloading, done, total, file.rel);
    }
}

struct UploadContext {
    Drive& drive;
    const Reporter& report;
    std::uint32_t total;
    std::uint32_t done = 0;
    bool prune_ok = true;
};

// cloud_upload_tree() + cloud_prune_extras(): upload every file (updating the ones that exist),
// create the folders, then move to the trash what the cloud has and the save does not. Pruning is
// what lets a file the game deleted stay deleted; without it the file would come back on the next
// download.
void UploadDirectory(UploadContext& context, const std::string& cloud_folder_id,
                     const fs::path& dir, bool is_root) {
    const std::vector<LocalEntry> locals = ScanLevel(dir, is_root, std::string());
    const std::vector<CloudItem> remote = context.drive.ListChildren(cloud_folder_id);

    for (const LocalEntry& entry : locals) {
        if (entry.is_dir) {
            std::string sub_id;
            for (const CloudItem& item : remote) {
                if (item.is_folder && item.name == entry.name) {
                    sub_id = item.id;
                    break;
                }
            }
            if (sub_id.empty()) {
                sub_id = context.drive.CreateFolder(cloud_folder_id, entry.name);
            }
            UploadDirectory(context, sub_id, entry.full, false);
        } else {
            std::string existing_id;
            for (const CloudItem& item : remote) {
                if (!item.is_folder && item.name == entry.name) {
                    existing_id = item.id;
                    break;
                }
            }
            context.drive.Upload(cloud_folder_id, entry.name, entry.full, entry.size, existing_id);
            ++context.done;
            context.report.Emit(SyncStage::Uploading, context.done, context.total, entry.name);
        }
    }

    for (const CloudItem& item : remote) {
        bool wanted = false;
        for (const LocalEntry& entry : locals) {
            if (entry.name == item.name && entry.is_dir == item.is_folder) {
                wanted = true;
                break;
            }
        }
        if (wanted) {
            continue;
        }
        context.report.Emit(SyncStage::Cleaning, 0, 0, item.name);
        if (!context.drive.Trash(item.id)) {
            Log("could not move '" + item.name + "' to the trash");
            context.prune_ok = false;
        }
    }
}

// core/syncjob.c write_over_save(): the save ends up EQUAL to the cloud copy. Before anything is
// touched the Xbox save is copied to backup_dir, and the result is read back and compared; if any
// step fails the previous save is restored. Eden's bookkeeping file stays where it is.
void ReplaceLocalSave(const fs::path& local_dir, const fs::path& staged_dir,
                      const fs::path& backup_dir, std::uint64_t expected_fingerprint) {
    const LocalTree before = ScanTree(local_dir, true);
    bool backed_up = false;
    if (before.files > 0) {
        RemoveTree(backup_dir);
        CopyTree(local_dir, backup_dir, true);
        backed_up = true;
    }
    try {
        std::error_code ec;
        fs::create_directories(local_dir, ec);
        ClearDirectory(local_dir, true);
        CopyTree(staged_dir, local_dir, false);
        if (FingerprintTree(ScanTree(local_dir, true)) != expected_fingerprint) {
            throw SyncError("the written save does not match the cloud copy");
        }
    } catch (const std::exception& failure) {
        Log(std::string("writing the cloud save failed (") + failure.what() +
            "); putting the previous Xbox save back");
        try {
            ClearDirectory(local_dir, true);
            if (backed_up) {
                CopyTree(backup_dir, local_dir, false);
            }
        } catch (const std::exception& restore_failure) {
            Log(std::string("could not put the previous Xbox save back: ") +
                restore_failure.what() + "; it is in " + Utf8FromPath(backup_dir));
        }
        throw SyncError(std::string("could not write the cloud save into the Eden save folder: ") +
                        failure.what());
    }
}

enum class Mode { Reconcile, ForceUpload, ForceDownload };

class SyncEngine {
public:
    SyncEngine(const Env& env, Drive& drive, const Reporter& report, Target target,
               std::string game_name)
        : env_(env), drive_(drive), report_(report), target_(std::move(target)),
          game_name_(std::move(game_name)) {}

    void Run(Mode mode, SyncResult& result) {
        report_.Emit(SyncStage::Connecting, 0, 0, std::string());
        root_id_ = drive_.EnsureFolder(std::string(), RootFolderName);
        saved_ = LoadState(env_, target_.key);

        if (!game_name_.empty()) {
            computed_path_ = BuildCloudFolderPath(game_name_, target_.owner);
        } else if (saved_.folder.find('/') != std::string::npos) {
            computed_path_ = saved_.folder; // no name this time: keep using the folder used before
        }
        if (computed_path_.empty()) {
            throw SyncError("the game name is not known yet: pass it to the sync call "
                            "(GameNameFromNames over the NACP names)");
        }
        cloud_ = LocateCloudFolder(drive_, root_id_, saved_.folder, computed_path_,
                                   [this] { return !OtherSavesExist(env_, target_); });
        result.cloud_folder = cloud_.found ? cloud_.path : computed_path_;
        Log("save " + target_.key + " -> cloud folder '" + result.cloud_folder + "' (" +
            (cloud_.found ? "found" : "not there yet") + ")");
        if (!cloud_.found) {
            LogGameFolders(drive_, root_id_, computed_path_);
        }

        report_.Emit(SyncStage::Comparing, 0, 0, std::string());
        local_ = ScanTree(target_.local_dir, true);
        result.local_files = local_.files;
        result.local_bytes = local_.bytes;
        result.local_modified = local_.newest;
        local_fp_ = FingerprintTree(local_);

        if (mode == Mode::ForceUpload) {
            if (local_.files == 0) {
                throw SyncError("there is no Xbox save to upload");
            }
            UploadLocal(result);
        } else {
            DownloadCloudCopy(mode, result);
        }
        report_.Emit(SyncStage::Finished, 0, 0, std::string());
    }

private:
    // Reconcile and ForceDownload: bring the cloud copy to the staging folder and act on it.
    void DownloadCloudCopy(Mode mode, SyncResult& result) {
        const ScopedCleanup cleanup(target_.staging_dir);
        bool cloud_empty = true;
        std::uint64_t cloud_fp = 0;
        if (cloud_.found) {
            const CloudSnapshot snapshot = ListCloudTree(drive_, cloud_.folder_id);
            result.cloud_files = static_cast<std::uint32_t>(snapshot.files.size());
            result.cloud_bytes = snapshot.bytes;
            result.cloud_modified = snapshot.newest;
            if (!snapshot.files.empty()) {
                cloud_empty = false;
                DownloadSnapshot(drive_, snapshot, target_.staging_dir, report_);
                cloud_fp = FingerprintTree(ScanTree(target_.staging_dir, false));
            }
        }

        if (mode == Mode::ForceDownload) {
            if (cloud_empty) {
                throw SyncError("the cloud has no save for this game");
            }
            ApplyCloud(cloud_fp, result);
            return;
        }

        const SyncAction action = Decide(local_.files == 0, cloud_empty, local_fp_, cloud_fp,
                                         saved_.has_fingerprint, saved_.fingerprint);
        switch (action) {
        case SyncAction::Nothing:
            result.status = SyncStatus::NothingToSync;
            result.message = "no save here and none in the cloud";
            break;
        case SyncAction::Upload:
            UploadLocal(result);
            break;
        case SyncAction::Download:
            ApplyCloud(cloud_fp, result);
            break;
        case SyncAction::Equal:
            StoreState(env_, target_.key, local_fp_, cloud_.path);
            result.status = SyncStatus::UpToDate;
            result.message = "already in sync";
            break;
        case SyncAction::Conflict:
            result.status = SyncStatus::Conflict;
            result.message =
                saved_.has_fingerprint
                    ? "both the Xbox save and the cloud save changed since the last sync"
                    : "there is a save on both sides and no earlier sync to tell which is newer";
            break;
        default:
            throw SyncError("internal error: unknown sync action");
        }
    }

    // The Xbox save goes to the cloud: the first-sync branch and the only-this-side-changed
    // branch of syncjob_sync_title().
    void UploadLocal(SyncResult& result) {
        const std::string path = cloud_.found ? cloud_.path : computed_path_;
        if (path.empty()) {
            throw SyncError("there is no cloud folder name to upload to");
        }
        const std::string folder_id =
            cloud_.found ? cloud_.folder_id : EnsureFolderPath(drive_, root_id_, path);
        UploadContext context{drive_, report_, local_.files};
        report_.Emit(SyncStage::Uploading, 0, local_.files, std::string());
        UploadDirectory(context, folder_id, target_.local_dir, true);
        cloud_.found = true;
        cloud_.folder_id = folder_id;
        cloud_.path = path;
        result.cloud_folder = path;
        result.status = SyncStatus::Uploaded;
        // The marker means "both sides were equal at this point". If the cleanup failed they are
        // not, so the marker is withheld and the next sync asks instead of guessing.
        if (context.prune_ok) {
            StoreState(env_, target_.key, local_fp_, path);
            result.message = "the Xbox save was uploaded";
        } else {
            StoreState(env_, target_.key, std::nullopt, path);
            result.message =
                "uploaded, but some old cloud files could not be removed; the next sync will ask";
        }
        result.cloud_files = local_.files;
        result.cloud_bytes = local_.bytes;
        result.cloud_modified = NowUnix();
    }

    // The cloud save replaces the Xbox save.
    void ApplyCloud(std::uint64_t cloud_fp, SyncResult& result) {
        report_.Emit(SyncStage::Applying, 0, 0, std::string());
        ReplaceLocalSave(target_.local_dir, target_.staging_dir, target_.backup_dir, cloud_fp);
        StoreState(env_, target_.key, cloud_fp, cloud_.path);
        result.status = SyncStatus::Downloaded;
        result.message = "the cloud save was written to the Xbox";
        result.local_files = result.cloud_files;
        result.local_bytes = result.cloud_bytes;
        result.local_modified = NowUnix();
    }

    const Env& env_;
    Drive& drive_;
    const Reporter& report_;
    Target target_;
    std::string game_name_;
    std::string root_id_;
    SavedState saved_;
    std::string computed_path_;
    CloudLocation cloud_;
    LocalTree local_;
    std::uint64_t local_fp_ = 0;
};

const char* StatusName(SyncStatus status) {
    switch (status) {
    case SyncStatus::Failed:
        return "failed";
    case SyncStatus::NotConfigured:
        return "not-configured";
    case SyncStatus::NotSignedIn:
        return "not-signed-in";
    case SyncStatus::NothingToSync:
        return "nothing-to-sync";
    case SyncStatus::UpToDate:
        return "up-to-date";
    case SyncStatus::Uploaded:
        return "uploaded";
    case SyncStatus::Downloaded:
        return "downloaded";
    case SyncStatus::Conflict:
        return "conflict";
    default:
        return "unknown";
    }
}

// Everything between "a title id came in" and "a SyncResult goes out", with every failure turned
// into a result. Nothing in here writes to the Xbox save unless a decision above allowed it.
SyncResult RunSync(const Env& env, std::uint64_t title_id, const ProgressCallback& progress,
                   const std::string& game_name, Mode mode, const char* trigger) {
    SyncResult result;
    const Reporter report(progress);
    try {
        if (title_id == 0) {
            throw SyncError("invalid title id");
        }
        Config config;
        std::string reason;
        if (!LoadConfig(env, config, reason)) {
            throw NotConfiguredError(reason);
        }
        const std::string refresh_token = ReadRefreshToken(env);
        if (refresh_token.empty()) {
            throw NotSignedInError("not signed in to Google");
        }
        ActiveUser user;
        if (!env.resolve_user || !env.resolve_user(user)) {
            throw SyncError("cannot tell which Eden user is active");
        }
        Target target = BuildTarget(env, title_id, user, config);
        Log(std::string(trigger) + " " + target.title_hex + " user " + user.dir_name);

        report.Emit(SyncStage::Connecting, 0, 0, std::string());
        std::string access_token;
        std::string detail;
        const RefreshOutcome refreshed =
            RefreshAccessToken(env, config, refresh_token, access_token, detail);
        if (refreshed == RefreshOutcome::Revoked) {
            // The token will never work again; keeping it would make every sync fail like a dead
            // network.
            RemoveRefreshToken(env);
            throw NotSignedInError("Google no longer accepts the saved sign-in");
        }
        if (refreshed != RefreshOutcome::Ok) {
            throw SyncError("cannot get a Google access token: " + detail);
        }

        Drive drive(env, access_token);
        SyncEngine engine(env, drive, report, std::move(target), game_name);
        engine.Run(mode, result);
    } catch (const NotSignedInError& failure) {
        result.status = SyncStatus::NotSignedIn;
        result.message = failure.what();
    } catch (const NotConfiguredError& failure) {
        result.status = SyncStatus::NotConfigured;
        result.message = failure.what();
    } catch (const SyncError& failure) {
        result.status = SyncStatus::Failed;
        result.message = failure.what();
    } catch (const winrt::hresult_error& failure) {
        result.status = SyncStatus::Failed;
        result.message = winrt::to_string(failure.message());
    } catch (const std::exception& failure) {
        result.status = SyncStatus::Failed;
        result.message = failure.what();
    }
    Log(std::string(trigger) + " " + Hex16(title_id) + ": " + StatusName(result.status) + " - " +
        result.message + " (xbox " + std::to_string(result.local_files) + " files, cloud " +
        std::to_string(result.cloud_files) + " files)");
    return result;
}

SyncResult RunPublic(std::uint64_t title_id, const ProgressCallback& progress,
                     const std::string& game_name, Mode mode, const char* trigger) {
    SyncResult failed;
    const std::lock_guard<std::mutex> guard(g_sync_mutex);
    try {
        WinrtTransport transport;
        const Env env = MakeEnv(&transport);
        return RunSync(env, title_id, progress, game_name, mode, trigger);
    } catch (const winrt::hresult_error& failure) {
        failed.message = winrt::to_string(failure.message());
    } catch (const std::exception& failure) {
        failed.message = failure.what();
    }
    Log(std::string(trigger) + ": " + failed.message);
    return failed;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

bool IsConfigured() {
    try {
        const Env env = MakeEnv(nullptr);
        Config config;
        std::string reason;
        return LoadConfig(env, config, reason);
    } catch (...) {
        return false;
    }
}

bool IsSignedIn() {
    try {
        const Env env = MakeEnv(nullptr);
        return !ReadRefreshToken(env).empty();
    } catch (...) {
        return false;
    }
}

void SignOut() {
    g_login_cancel.store(true);
    EndLogin();
    try {
        const Env env = MakeEnv(nullptr);
        RemoveRefreshToken(env);
        Log("signed out");
    } catch (...) {
    }
}

DeviceLogin StartDeviceLogin() {
    DeviceLogin login;
    try {
        WinrtTransport transport;
        const Env env = MakeEnv(&transport);
        login = StartLogin(env);
    } catch (const winrt::hresult_error& failure) {
        login.ok = false;
        login.error = winrt::to_string(failure.message());
    } catch (const std::exception& failure) {
        login.ok = false;
        login.error = failure.what();
    }
    if (!login.ok) {
        Log("sign-in could not start: " + login.error);
    }
    return login;
}

LoginResult PollDeviceLogin() {
    try {
        WinrtTransport transport;
        const Env env = MakeEnv(&transport);
        return PollLogin(env);
    } catch (const winrt::hresult_error& failure) {
        Log("sign-in poll failed: " + winrt::to_string(failure.message()));
    } catch (const std::exception& failure) {
        Log(std::string("sign-in poll failed: ") + failure.what());
    }
    EndLogin();
    return LoginResult::Failed;
}

void CancelDeviceLogin() {
    g_login_cancel.store(true);
}

SyncResult SyncBeforeBoot(std::uint64_t title_id, const ProgressCallback& progress,
                          const std::string& game_name) {
    return RunPublic(title_id, progress, game_name, Mode::Reconcile, "before boot");
}

SyncResult SyncAfterExit(std::uint64_t title_id, const ProgressCallback& progress,
                         const std::string& game_name) {
    return RunPublic(title_id, progress, game_name, Mode::Reconcile, "after exit");
}

SyncResult ResolveConflict(std::uint64_t title_id, ConflictChoice choice,
                           const ProgressCallback& progress, const std::string& game_name) {
    const bool keep_xbox = choice == ConflictChoice::KeepXbox;
    return RunPublic(title_id, progress, game_name,
                     keep_xbox ? Mode::ForceUpload : Mode::ForceDownload,
                     keep_xbox ? "resolve (keep Xbox)" : "resolve (keep cloud)");
}

std::string GameNameFromNames(const std::vector<std::string>& nacp_application_names) {
    const std::size_t count = std::min(nacp_application_names.size(), NacpLanguageCount);
    for (std::size_t i = 0; i < count; ++i) {
        if (!nacp_application_names[i].empty()) {
            return nacp_application_names[i].substr(0, NacpNameBytes);
        }
    }
    return std::string();
}

} // namespace EdenXbox::SaveSync
