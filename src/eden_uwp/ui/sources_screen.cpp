// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden headers #undef the A/W macros of the Win32 calls they
// name their methods after.
#include <windows.h>

// wingdi.h defines GetObject as a macro for GetObjectW, which would rename JsonValue::GetObject().
#ifdef GetObject
#undef GetObject
#endif

#include "eden_uwp/ui/sources_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/scope_exit.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/game_download.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;

using D2D1::RectF;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Storage::FileIO;
using winrt::Windows::Storage::KnownFolders;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;
using winrt::Windows::Web::Http::HttpClient;

// Layout, in canvas units. The rows follow the USB import list.
constexpr float kMargin = kScreenMargin;
constexpr float kRight = kCanvasWidth - kScreenMargin;
constexpr float kTitleTop = 150.0f;
constexpr float kTitleHeight = 76.0f;
constexpr float kSubTop = 232.0f;
constexpr float kSubHeight = 30.0f;
constexpr float kListTop = 330.0f;
constexpr float kRowHeight = 104.0f;
constexpr float kRowPitch = 110.0f;
constexpr float kRowRadius = 22.0f;
constexpr float kRowPadding = 28.0f;
constexpr float kStatWidth = 220.0f;
constexpr int kVisibleRows = 5;

constexpr float kSheetLeft = 420.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kConfirmTop = 340.0f;
constexpr float kConfirmHeight = 360.0f;
constexpr float kWorkTop = 340.0f;
constexpr float kWorkHeight = 360.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(4200);
constexpr auto kRateWindow = std::chrono::milliseconds(500);

constexpr std::size_t kMaxEntries = 5000;     // per source, so a huge index cannot eat the memory
constexpr std::size_t kMaxDirectories = 32;   // indexes followed one level down
constexpr auto kRequestTimeout = std::chrono::seconds(30);

struct Source {
    std::string name;
    std::string url;
};

enum class Kind { Other, Game, Update, Dlc };

struct Entry {
    std::wstring name; // what the source calls it, for display
    std::wstring file; // the name it is saved under
    std::string url;   // absolute, without the #name fragment
    std::uint64_t size = 0;
    Kind kind = Kind::Other;
};

enum class Failure { None, Unreachable, Unsupported, SourceError };

struct FetchResult {
    std::vector<Entry> entries;
    std::wstring message; // the source's own "success" or "error" text
    Failure failure = Failure::None;
};

struct ImportResult {
    std::vector<Source> sources;
};

// State a detached worker shares with the screen. The worker owns a reference, so leaving the
// screen never has to wait for a request that is still in flight.
template <typename Result>
struct Task {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    Result result; // written by the worker before `done` is set, read by the screen after
};

// ---- Small string helpers (UTF-8 unless noted) ----

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool IsHttpUrl(const std::string& url) {
    const std::string lower = Lower(url.substr(0, 8));
    const bool http = StartsWith(lower, "http://") && url.size() > 7;
    const bool https = StartsWith(lower, "https://") && url.size() > 8;
    if (!http && !https) {
        return false;
    }
    return std::none_of(url.begin(), url.end(),
                        [](unsigned char c) { return c <= ' ' || c == 0x7F; });
}

// The text between "://" and the next "/", "?" or "#", without credentials.
std::string HostOf(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t start = scheme + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority.erase(0, at + 1);
    }
    return authority;
}

// For the diagnostic log and the list: no credentials and no query string, which may carry a token.
std::string RedactedUrl(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t path = url.find_first_of("/?#", scheme + 3);
    std::string result = url.substr(0, scheme + 3) + HostOf(url);
    if (path != std::string::npos && url[path] == '/') {
        const std::size_t cut = url.find_first_of("?#", path);
        result += url.substr(path, cut == std::string::npos ? cut : cut - path);
    }
    return result;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 &&
            HexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string Utf8(const std::wstring& text) {
    return winrt::to_string(text);
}

// ---- Classifying a name ----

// Tinfoil-style names carry "[<16 hex title id>]" and "[v<version>]". A title id ending in 000 is
// a base game, 800 an update, anything else add-on content. Names without a title id fall back to
// the version tag and to the word DLC.
Kind Classify(const std::string& name) {
    const std::string lower = Lower(name);
    std::string title_id;
    long long version = -1;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        if (lower[i] != '[') {
            continue;
        }
        const std::size_t close = lower.find(']', i);
        if (close == std::string::npos) {
            break;
        }
        const std::string tag = lower.substr(i + 1, close - i - 1);
        const auto all = [&tag](std::size_t from, auto predicate) {
            return std::all_of(tag.begin() + static_cast<std::ptrdiff_t>(from), tag.end(),
                               [&predicate](char c) { return predicate(c); });
        };
        if (tag.size() == 16 && all(0, [](char c) { return HexValue(c) >= 0; })) {
            title_id = tag;
        } else if (tag.size() >= 2 && tag.size() <= 10 && tag[0] == 'v' &&
                   all(1, [](char c) { return c >= '0' && c <= '9'; })) {
            version = std::stoll(tag.substr(1));
        }
        i = close;
    }
    if (!title_id.empty()) {
        const std::string tail = title_id.substr(13);
        if (tail == "000") {
            return version > 0 ? Kind::Update : Kind::Game;
        }
        return tail == "800" ? Kind::Update : Kind::Dlc;
    }
    if (lower.find("dlc") != std::string::npos) {
        return Kind::Dlc;
    }
    if (version > 0) {
        return Kind::Update;
    }
    return version == 0 ? Kind::Game : Kind::Other;
}

const wchar_t* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::Game:
        return Tr(Text::SrcTypeGame);
    case Kind::Update:
        return Tr(Text::SrcTypeUpdate);
    case Kind::Dlc:
        return Tr(Text::SrcTypeDlc);
    case Kind::Other:
        break;
    }
    return L"";
}

bool IsGameExtension(const std::wstring& extension) {
    return extension == L".nsp" || extension == L".nsz" || extension == L".xci" ||
           extension == L".xcz";
}

std::wstring LowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::wstring ExtensionOf(const std::wstring& name) {
    const std::size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : LowerWide(name.substr(dot));
}

// The name a download is saved under: characters Windows rejects become "_", and a name without a
// game extension borrows the one of the URL so the library recognizes the file.
std::wstring DiskName(const std::wstring& display_name, const std::string& clean_url) {
    std::wstring name = display_name;
    for (wchar_t& c : name) {
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) {
            c = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.pop_back();
    }
    if (name.size() > 180) {
        name.resize(180);
    }
    if (!IsGameExtension(ExtensionOf(name))) {
        const std::size_t query = clean_url.find_first_of("?#");
        const std::wstring url_name = Widen(UrlDecode(clean_url.substr(0, query)));
        const std::wstring extension = ExtensionOf(url_name);
        if (IsGameExtension(extension)) {
            name += extension;
        }
    }
    return name.empty() ? std::wstring(L"download") : name;
}

// ---- JSON access that tolerates missing and null fields ----

std::string StringAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::String) {
            return winrt::to_string(value.GetString());
        }
    }
    return {};
}

double NumberAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Number) {
            return value.GetNumber();
        }
        if (value.ValueType() == JsonValueType::String) {
            return std::atof(winrt::to_string(value.GetString()).c_str());
        }
    }
    return 0.0;
}

JsonArray ArrayAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Array) {
            return value.GetArray();
        }
    }
    return JsonArray{};
}

bool ParseObject(const std::string& text, JsonObject& object) {
    try {
        return JsonObject::TryParse(winrt::to_hstring(text), object);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// ---- The saved list ----

fs::path SourcesFile(const fs::path& local_state) {
    return local_state / L"sources.json";
}

std::vector<Source> LoadSources(const fs::path& local_state) {
    std::vector<Source> sources;
    try {
        std::ifstream in(SourcesFile(local_state), std::ios::binary);
        if (!in) {
            return sources;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        JsonObject root;
        if (!ParseObject(text, root)) {
            return sources;
        }
        for (const IJsonValue& value : ArrayAt(root, L"sources")) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject item = value.as<winrt::Windows::Data::Json::JsonObject>();
            Source source;
            source.name = StringAt(item, L"name");
            source.url = StringAt(item, L"url");
            if (IsHttpUrl(source.url)) {
                if (source.name.empty()) {
                    source.name = HostOf(source.url);
                }
                sources.push_back(std::move(source));
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_LOAD failed " + winrt::to_string(error.message()));
    }
    return sources;
}

bool SaveSources(const fs::path& local_state, const std::vector<Source>& sources) {
    try {
        JsonArray list;
        for (const Source& source : sources) {
            JsonObject item;
            item.SetNamedValue(L"name", JsonValue::CreateStringValue(winrt::to_hstring(source.name)));
            item.SetNamedValue(L"url", JsonValue::CreateStringValue(winrt::to_hstring(source.url)));
            list.Append(item);
        }
        JsonObject root;
        root.SetNamedValue(L"sources", list);
        std::error_code ec;
        fs::create_directories(local_state, ec);
        const fs::path file = SourcesFile(local_state);
        const fs::path temporary = local_state / L"sources.json.tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << winrt::to_string(root.Stringify());
            out.flush();
            if (!out) {
                Diagnostic("SOURCES_SAVE write failed");
                return false;
            }
        }
        fs::rename(temporary, file, ec);
        if (ec) {
            // rename refused to replace an existing file: remove it and try once more.
            fs::remove(file, ec);
            ec.clear();
            fs::rename(temporary, file, ec);
        }
        if (ec) {
            Diagnostic("SOURCES_SAVE rename failed " + ec.message());
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_SAVE failed " + winrt::to_string(error.message()));
        return false;
    }
}

// False when the address is already in the list (compared without case or a trailing slash).
bool AddSource(std::vector<Source>& sources, Source source) {
    const auto key = [](const std::string& url) {
        std::string lower = Lower(url);
        while (!lower.empty() && lower.back() == '/') {
            lower.pop_back();
        }
        return lower;
    };
    const std::string wanted = key(source.url);
    for (const Source& existing : sources) {
        if (key(existing.url) == wanted) {
            return false;
        }
    }
    if (source.name.empty()) {
        source.name = HostOf(source.url);
    }
    sources.push_back(std::move(source));
    return true;
}

// sources.txt: one URL per line, or "name|url". Blank lines and lines starting with # are skipped.
std::vector<Source> ParseSourcesText(const std::string& text) {
    std::vector<Source> found;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF && line.size() >= 3 &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = Trim(line.substr(3)); // a UTF-8 byte order mark
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Source source;
        const std::size_t bar = line.find('|');
        if (!IsHttpUrl(line) && bar != std::string::npos) {
            source.name = Trim(line.substr(0, bar));
            source.url = Trim(line.substr(bar + 1));
        } else {
            source.url = line;
        }
        if (IsHttpUrl(source.url)) {
            found.push_back(std::move(source));
        }
    }
    return found;
}

// Looks for sources.txt at the root of each removable drive and in its switch\ folder.
ImportResult ScanDrivesForSources(const std::atomic<bool>& cancel) {
    ImportResult result;
    try {
        const auto drives = AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(),
                                         std::chrono::seconds(5));
        if (!drives) {
            return result;
        }
        for (const auto& drive : *drives) {
            for (const wchar_t* relative : {L"sources.txt", L"switch\\sources.txt"}) {
                if (cancel.load()) {
                    return result;
                }
                try {
                    const auto item =
                        AwaitBounded(drive.TryGetItemAsync(relative), std::chrono::seconds(3));
                    const auto file = item && *item ? item->try_as<StorageFile>() : nullptr;
                    if (!file) {
                        continue;
                    }
                    const auto text =
                        AwaitBounded(FileIO::ReadTextAsync(file), std::chrono::seconds(10));
                    if (!text) {
                        continue;
                    }
                    for (Source& source : ParseSourcesText(winrt::to_string(*text))) {
                        result.sources.push_back(std::move(source));
                    }
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("SOURCES_IMPORT skip " + winrt::to_string(error.message()));
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_IMPORT failed " + winrt::to_string(error.message()));
    }
    Diagnostic("SOURCES_IMPORT found=" + std::to_string(result.sources.size()));
    return result;
}

// ---- Reading a source ----

// GETs a text body. False, with a log line, on a network error or an unsuccessful status.
bool HttpGetText(const HttpClient& client, const std::string& url, std::string& body) {
    try {
        const auto response =
            AwaitBounded(client.GetAsync(Uri{winrt::to_hstring(url)}), kRequestTimeout);
        if (!response) {
            Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " timed out");
            return false;
        }
        if (!response->IsSuccessStatusCode()) {
            Diagnostic(fmt::format("SOURCES_FETCH {} status={}", RedactedUrl(url),
                                   static_cast<int>(response->StatusCode())));
            return false;
        }
        const auto text =
            AwaitBounded(response->Content().ReadAsStringAsync(), kRequestTimeout);
        if (!text) {
            Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " body timed out");
            return false;
        }
        body = winrt::to_string(*text);
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " failed " +
                   winrt::to_string(error.message()));
        return false;
    }
}

// An entry's address as the index wrote it (absolute, or relative to the index) made absolute.
std::optional<std::string> Resolve(const std::string& base, const std::string& reference) {
    const std::string ref = Trim(reference);
    if (ref.empty()) {
        return std::nullopt;
    }
    if (IsHttpUrl(ref)) {
        return ref;
    }
    try {
        const Uri uri{winrt::to_hstring(base), winrt::to_hstring(ref)};
        std::string absolute = winrt::to_string(uri.AbsoluteUri());
        if (IsHttpUrl(absolute)) {
            return absolute;
        }
    } catch (const winrt::hresult_error&) {
        // Not a usable address: skipped below.
    }
    return std::nullopt;
}

// Splits "<url>#<name>" into the address to download and the display name (decoded).
void SplitFragment(const std::string& url, std::string& clean, std::string& fragment) {
    const std::size_t hash = url.find('#');
    clean = url.substr(0, hash);
    fragment = hash == std::string::npos ? std::string() : UrlDecode(url.substr(hash + 1));
}

std::optional<Entry> MakeEntry(const std::string& base, const std::string& reference,
                               double size) {
    const auto absolute = Resolve(base, reference);
    if (!absolute) {
        return std::nullopt;
    }
    Entry entry;
    std::string name;
    SplitFragment(*absolute, entry.url, name);
    if (name.empty()) {
        // The last path segment, decoded. "http://host" has none.
        const std::string no_query = entry.url.substr(0, entry.url.find('?'));
        const std::size_t scheme = no_query.find("://");
        const std::size_t last = no_query.find_last_of('/');
        if (last != std::string::npos && (scheme == std::string::npos || last > scheme + 2)) {
            name = UrlDecode(no_query.substr(last + 1));
        }
    }
    if (name.empty()) {
        name = HostOf(entry.url);
    }
    entry.kind = Classify(name);
    entry.name = Widen(name);
    entry.file = DiskName(entry.name, entry.url);
    entry.size = size > 0.0 ? static_cast<std::uint64_t>(size) : 0;
    return entry;
}

struct ParsedIndex {
    std::vector<Entry> files;
    std::vector<std::string> directories;
    std::wstring success;
    std::wstring error;
};

// False when the body is not a JSON object, which is what an encrypted Tinfoil index or a web page
// looks like.
bool ParseIndex(const std::string& body, const std::string& base, ParsedIndex& index) {
    JsonObject root;
    if (!ParseObject(body, root)) {
        return false;
    }
    for (const IJsonValue& value : ArrayAt(root, L"files")) {
        if (index.files.size() >= kMaxEntries) {
            break;
        }
        std::optional<Entry> entry;
        if (value.ValueType() == JsonValueType::String) {
            entry = MakeEntry(base, winrt::to_string(value.GetString()), 0.0);
        } else if (value.ValueType() == JsonValueType::Object) {
            const JsonObject item = value.as<winrt::Windows::Data::Json::JsonObject>();
            entry = MakeEntry(base, StringAt(item, L"url"), NumberAt(item, L"size"));
        }
        if (entry) {
            index.files.push_back(std::move(*entry));
        }
    }
    for (const IJsonValue& value : ArrayAt(root, L"directories")) {
        std::string reference;
        if (value.ValueType() == JsonValueType::String) {
            reference = winrt::to_string(value.GetString());
        } else if (value.ValueType() == JsonValueType::Object) {
            reference = StringAt(value.as<winrt::Windows::Data::Json::JsonObject>(), L"url");
        }
        if (const auto absolute = Resolve(base, reference)) {
            index.directories.push_back(*absolute);
        }
    }
    index.success = Widen(StringAt(root, L"success"));
    index.error = Widen(StringAt(root, L"error"));
    return true;
}

FetchResult FetchSource(const std::string& url, const std::atomic<bool>& cancel) {
    FetchResult result;
    HttpClient client;
    // A failure to add the header is not worth failing the request for.
    void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
    std::string body;
    if (!HttpGetText(client, url, body)) {
        result.failure = Failure::Unreachable;
        return result;
    }
    ParsedIndex index;
    if (!ParseIndex(body, url, index)) {
        Diagnostic("SOURCES_FETCH " + RedactedUrl(url) + " unsupported format");
        result.failure = Failure::Unsupported;
        return result;
    }
    result.entries = std::move(index.files);
    result.message = !index.success.empty() ? index.success : index.error;
    // One level down, and never the same index twice.
    std::set<std::string> visited{url};
    std::size_t followed = 0;
    for (const std::string& directory : index.directories) {
        if (cancel.load() || followed >= kMaxDirectories || result.entries.size() >= kMaxEntries) {
            break;
        }
        if (!visited.insert(directory).second) {
            continue;
        }
        ++followed;
        std::string sub_body;
        ParsedIndex sub;
        if (!HttpGetText(client, directory, sub_body) || !ParseIndex(sub_body, directory, sub)) {
            continue;
        }
        for (Entry& entry : sub.files) {
            if (result.entries.size() >= kMaxEntries) {
                break;
            }
            result.entries.push_back(std::move(entry));
        }
    }
    std::stable_sort(result.entries.begin(), result.entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return LowerWide(a.name) < LowerWide(b.name);
                     });
    if (result.entries.empty() && !index.error.empty()) {
        result.failure = Failure::SourceError;
    }
    Diagnostic(fmt::format("SOURCES_FETCH {} files={} dirs={}", RedactedUrl(url),
                           result.entries.size(), index.directories.size()));
    return result;
}

// Runs `body(task)` on a detached thread with its own MTA apartment.
template <typename Result, typename Body>
std::shared_ptr<Task<Result>> Spawn(Body body) {
    auto task = std::make_shared<Task<Result>>();
    std::thread([task, body = std::move(body)]() mutable {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment {
                ~Apartment() {
                    winrt::uninit_apartment();
                }
            } apartment;
            body(*task);
        } catch (const winrt::hresult_error& error) {
            Diagnostic("SOURCES worker failed " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("SOURCES worker failed ") + error.what());
        }
        task->done.store(true);
    }).detach();
    return task;
}

enum class Mode { List, Keyboard, Importing, Browse, Download, Confirm };
enum class Outcome { Pending, Done, Failed, Cancelled };

struct Cursor {
    int selected = 0;
    int top = 0;
};

class SourcesScreen {
public:
    SourcesScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                  const fs::path& local_state)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state) {}

    ~SourcesScreen() {
        if (fetch_) {
            fetch_->cancel.store(true);
        }
        if (import_) {
            import_->cancel.store(true);
        }
        cancel_.store(true);
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
    }

    // Returns true when the window was closed.
    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI sources screen open");
        sources_ = LoadSources(local_state_);
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        Diagnostic("UI sources screen closed");
        return closed_;
    }

private:
    // ---- State ----

    int ListRows() const { return 2 + static_cast<int>(sources_.size()); }
    int BrowseRows() const { return static_cast<int>(entries_.size()); }
    bool InBrowse() const { return mode_ == Mode::Browse || mode_ == Mode::Download; }
    Cursor& CurrentCursor() { return InBrowse() ? browse_cursor_ : list_cursor_; }
    int CurrentRows() const { return InBrowse() ? BrowseRows() : ListRows(); }
    bool Loading() const { return fetch_ && !fetch_->done.load(); }

    // Which source row is focused, or -1 on one of the two action rows.
    int FocusedSource() const { return list_cursor_.selected - 2; }

    void Toast(const std::wstring& text, Clock::time_point now) {
        if (text.empty()) {
            return;
        }
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    void SetMode(Mode mode) {
        mode_ = mode;
        scroll_ = Tween(static_cast<float>(CurrentCursor().top));
    }

    void OpenKeyboard(const std::wstring& initial) {
        entry_ = std::make_unique<TextEntry>(Tr(Text::SrcEnterUrl), initial);
        SetMode(Mode::Keyboard);
    }

    void StartBrowse() {
        const Source& source = sources_[static_cast<std::size_t>(FocusedSource())];
        browse_name_ = Widen(source.name);
        browse_url_ = source.url;
        LoadBrowse();
        browse_cursor_ = Cursor{};
        SetMode(Mode::Browse);
    }

    void LoadBrowse() {
        if (fetch_) {
            fetch_->cancel.store(true);
        }
        ++fetch_serial_;
        entries_.clear();
        fetch_message_.clear();
        failure_ = Failure::None;
        fetch_ = Spawn<FetchResult>([url = browse_url_](Task<FetchResult>& task) {
            task.result = FetchSource(url, task.cancel);
        });
    }

    void AdoptFetch() {
        if (!fetch_ || !fetch_->done.load() || fetch_adopted_ == fetch_serial_) {
            return;
        }
        fetch_adopted_ = fetch_serial_;
        entries_ = std::move(fetch_->result.entries);
        fetch_message_ = fetch_->result.message;
        failure_ = fetch_->result.failure;
        browse_cursor_.selected = std::clamp(browse_cursor_.selected, 0, std::max(BrowseRows() - 1, 0));
    }

    void StartImport() {
        import_ = Spawn<ImportResult>([](Task<ImportResult>& task) {
            task.result = ScanDrivesForSources(task.cancel);
        });
        SetMode(Mode::Importing);
    }

    void AdoptImport(Clock::time_point now) {
        if (!import_ || !import_->done.load()) {
            return;
        }
        int added = 0;
        for (Source& source : import_->result.sources) {
            if (AddSource(sources_, std::move(source))) {
                ++added;
            }
        }
        import_.reset();
        if (added > 0) {
            SaveSources(local_state_, sources_);
            Diagnostic("SOURCES_ADD imported=" + std::to_string(added));
            Toast(std::to_wstring(added) + L" " + Tr(Text::SrcImported), now);
        } else {
            Toast(Tr(Text::SrcImportNone), now);
        }
        SetMode(Mode::List);
    }

    void StartDownload(const Entry& entry) {
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        download_name_ = entry.name;
        cancel_.store(false);
        bytes_done_.store(0);
        bytes_total_.store(entry.size);
        rate_.store(0.0);
        outcome_.store(Outcome::Pending);
        const fs::path destination = local_state_ / L"games" / entry.file;
        download_thread_ = std::thread([this, url = entry.url, name = Utf8(entry.name),
                                        destination]() mutable {
            Outcome outcome = Outcome::Failed;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment {
                    ~Apartment() {
                        winrt::uninit_apartment();
                    }
                } apartment;
                Clock::time_point sample_at = Clock::now();
                std::uint64_t sample_bytes = 0;
                bool first = true;
                const auto progress = [&](std::uint64_t have, std::uint64_t total) {
                    bytes_done_.store(have);
                    bytes_total_.store(total);
                    const Clock::time_point at = Clock::now();
                    if (first) {
                        first = false;
                        sample_at = at;
                        sample_bytes = have;
                        return;
                    }
                    if (at - sample_at >= kRateWindow) {
                        const double seconds = std::chrono::duration<double>(at - sample_at).count();
                        rate_.store(static_cast<double>(have - sample_bytes) / seconds);
                        sample_at = at;
                        sample_bytes = have;
                    }
                };
                // The player's pick is not remembered anywhere: a partial file is found again by
                // its name, in games\ or in a drive's NXbox\games.
                fs::path target = destination;
                const bool ok = DownloadFile(url, target, [](const fs::path&) {}, progress, &cancel_);
                outcome = ok ? Outcome::Done : (cancel_.load() ? Outcome::Cancelled : Outcome::Failed);
            } catch (const winrt::hresult_error& error) {
                Diagnostic("SOURCES_DOWNLOAD worker failed " + winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                Diagnostic(std::string("SOURCES_DOWNLOAD worker failed ") + error.what());
            }
            Diagnostic("SOURCES_DOWNLOAD " + name +
                       (outcome == Outcome::Done        ? " ok"
                        : outcome == Outcome::Cancelled ? " cancelled"
                                                        : " failed"));
            outcome_.store(outcome);
        });
        SetMode(Mode::Download);
    }

    // ---- Input ----

    void Update(Clock::time_point now) {
        AdoptFetch();
        switch (mode_) {
        case Mode::Keyboard:
            UpdateKeyboard(now);
            return;
        case Mode::Importing:
            if (input_.Pressed(Button::B)) {
                import_->cancel.store(true);
                import_.reset();
                SetMode(Mode::List);
            } else {
                AdoptImport(now);
            }
            return;
        case Mode::Download:
            UpdateDownload(now);
            return;
        case Mode::Confirm:
            UpdateConfirm(now);
            return;
        case Mode::Browse:
            UpdateBrowse(now);
            break;
        case Mode::List:
            UpdateList(now);
            break;
        }
        Scroll(now);
    }

    void Scroll(Clock::time_point now) {
        Cursor& cursor = CurrentCursor();
        cursor.selected = std::clamp(cursor.selected, 0, std::max(CurrentRows() - 1, 0));
        if (cursor.selected < cursor.top) {
            cursor.top = cursor.selected;
        } else if (cursor.selected > cursor.top + kVisibleRows - 1) {
            cursor.top = cursor.selected - (kVisibleRows - 1);
        }
        cursor.top = std::max(cursor.top, 0);
        if (scroll_.Target() != static_cast<float>(cursor.top)) {
            scroll_.To(static_cast<float>(cursor.top), now, kDurationPanel);
        }
    }

    void MoveCursor(int rows) {
        Cursor& cursor = CurrentCursor();
        if (input_.Pressed(Button::Up)) {
            cursor.selected = std::max(cursor.selected - 1, 0);
        }
        if (input_.Pressed(Button::Down)) {
            cursor.selected = std::min(cursor.selected + 1, std::max(rows - 1, 0));
        }
    }

    void UpdateList(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            leaving_ = true;
            return;
        }
        MoveCursor(ListRows());
        if (input_.Pressed(Button::X) && FocusedSource() >= 0) {
            confirm_focus_ = 1; // the safe answer
            SetMode(Mode::Confirm);
            return;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        if (list_cursor_.selected == 0) {
            OpenKeyboard({});
        } else if (list_cursor_.selected == 1) {
            StartImport();
        } else {
            StartBrowse();
        }
        (void)now;
    }

    void UpdateKeyboard(Clock::time_point now) {
        const TextEntry::Result result = entry_->Update(input_);
        if (result == TextEntry::Result::Editing) {
            return;
        }
        const std::string typed = Trim(Utf8(entry_->Text()));
        if (result == TextEntry::Result::Cancelled || typed.empty()) {
            entry_.reset();
            SetMode(Mode::List);
            return;
        }
        if (!IsHttpUrl(typed)) {
            Toast(Tr(Text::SrcInvalidUrl), now);
            return; // the keyboard stays open with the text, to fix it
        }
        Source source;
        source.url = typed;
        if (!AddSource(sources_, std::move(source))) {
            Toast(Tr(Text::SrcDuplicate), now);
            entry_.reset();
            SetMode(Mode::List);
            return;
        }
        SaveSources(local_state_, sources_);
        Diagnostic("SOURCES_ADD " + RedactedUrl(typed));
        Toast(Tr(Text::SrcAdded), now);
        list_cursor_.selected = ListRows() - 1;
        entry_.reset();
        SetMode(Mode::List);
    }

    void UpdateConfirm(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            SetMode(Mode::List);
            return;
        }
        if (input_.Pressed(Button::Left) || input_.Pressed(Button::Right)) {
            confirm_focus_ = 1 - confirm_focus_;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        const int index = FocusedSource();
        if (confirm_focus_ == 0 && index >= 0 && index < static_cast<int>(sources_.size())) {
            Diagnostic("SOURCES_REMOVE " + RedactedUrl(sources_[static_cast<std::size_t>(index)].url));
            sources_.erase(sources_.begin() + index);
            SaveSources(local_state_, sources_);
            Toast(Tr(Text::SrcRemoved), now);
        }
        SetMode(Mode::List);
    }

    void UpdateBrowse(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            if (fetch_) {
                fetch_->cancel.store(true);
            }
            SetMode(Mode::List);
            return;
        }
        if (input_.Pressed(Button::X) && !Loading()) {
            LoadBrowse();
            return;
        }
        if (Loading() || failure_ != Failure::None) {
            return;
        }
        MoveCursor(BrowseRows());
        if (input_.Pressed(Button::A) && BrowseRows() > 0) {
            StartDownload(entries_[static_cast<std::size_t>(browse_cursor_.selected)]);
        }
        (void)now;
    }

    void UpdateDownload(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            cancel_.store(true); // the worker stops at the next chunk and keeps the partial file
        }
        const Outcome outcome = outcome_.load();
        if (outcome == Outcome::Pending) {
            return;
        }
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        Toast(outcome == Outcome::Done        ? Tr(Text::SrcDownloadDone)
              : outcome == Outcome::Cancelled ? Tr(Text::SrcDownloadCancelled)
                                              : Tr(Text::SrcDownloadFailed),
              now);
        SetMode(Mode::Browse);
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        DrawHeader();
        if (InBrowse()) {
            DrawBrowse(now);
        } else {
            DrawListRows(now);
        }
        DrawTabs(renderer_, 0, false);
        DrawClock(renderer_);
        switch (mode_) {
        case Mode::Keyboard:
            entry_->Draw(renderer_, now);
            DrawToast(now);
            return; // the keyboard draws its own hints
        case Mode::Confirm:
            DrawConfirm();
            break;
        case Mode::Importing:
            DrawBusy(Tr(Text::SrcImporting));
            break;
        case Mode::Download:
            DrawProgress();
            break;
        default:
            break;
        }
        DrawToast(now);
        DrawHints(renderer_, CurrentHints());
    }

    void DrawHeader() {
        const std::wstring title = InBrowse() ? browse_name_ : std::wstring(Tr(Text::SourcesRowTitle));
        renderer_.DrawString(title, Font::ModsTitle,
                             RectF(kMargin, kTitleTop, kRight, kTitleTop + kTitleHeight),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        std::wstring line = Tr(Text::SrcSubtitle);
        D2D1_COLOR_F color = Theme::kTextSecondary;
        if (InBrowse()) {
            if (Loading()) {
                line = Tr(Text::SrcLoading);
            } else if (failure_ == Failure::Unreachable) {
                line = Tr(Text::SrcUnreachable);
                color = Theme::kDangerText;
            } else if (failure_ == Failure::Unsupported) {
                line = Tr(Text::SrcUnsupported);
                color = Theme::kDangerText;
            } else if (failure_ == Failure::SourceError) {
                line = fetch_message_;
                color = Theme::kDangerText;
            } else {
                line = std::to_wstring(entries_.size()) + L" " + Tr(Text::SrcItems);
                if (!fetch_message_.empty()) {
                    line += L"  ·  " + fetch_message_;
                }
            }
        }
        renderer_.DrawString(line, Font::Meta, RectF(kMargin, kSubTop, kRight, kSubTop + kSubHeight),
                             color, HAlign::Left, VAlign::Middle);
    }

    struct RowText {
        std::wstring title;
        std::wstring sub;
        std::wstring stat;
    };

    void DrawListRows(Clock::time_point now) {
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + static_cast<float>(kVisibleRows) * kRowPitch - 1.0f));
        for (int i = 0; i < ListRows(); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f ||
                y > kListTop + static_cast<float>(kVisibleRows) * kRowPitch) {
                continue;
            }
            RowText row;
            if (i == 0) {
                row = {Tr(Text::SrcAdd), Tr(Text::SrcAddHint), L""};
            } else if (i == 1) {
                row = {Tr(Text::SrcImportUsb), Tr(Text::SrcImportHint), L""};
            } else {
                const Source& source = sources_[static_cast<std::size_t>(i - 2)];
                row = {Widen(source.name), Widen(RedactedUrl(source.url)), L""};
            }
            DrawRow(row, i, y, mode_ == Mode::List && i == list_cursor_.selected);
        }
        renderer_.PopClip();
        if (sources_.empty()) {
            renderer_.DrawString(Tr(Text::SrcEmpty), Font::Body,
                                 RectF(kMargin, kListTop + 2.0f * kRowPitch + 20.0f, kRight,
                                       kListTop + 2.0f * kRowPitch + 140.0f),
                                 Theme::kTextSecondary);
        }
    }

    void DrawBrowse(Clock::time_point now) {
        if (Loading() || failure_ != Failure::None) {
            return;
        }
        if (entries_.empty()) {
            renderer_.DrawString(Tr(Text::SrcNoFiles), Font::Body,
                                 RectF(kMargin, kListTop + 20.0f, kRight, kListTop + 140.0f),
                                 Theme::kTextSecondary);
            return;
        }
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + static_cast<float>(kVisibleRows) * kRowPitch - 1.0f));
        for (int i = 0; i < BrowseRows(); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f ||
                y > kListTop + static_cast<float>(kVisibleRows) * kRowPitch) {
                continue;
            }
            const Entry& entry = entries_[static_cast<std::size_t>(i)];
            std::wstring sub = KindLabel(entry.kind);
            if (sub.empty()) {
                sub = ExtensionOf(entry.file);
                if (!sub.empty()) {
                    sub.erase(0, 1);
                    std::transform(sub.begin(), sub.end(), sub.begin(), [](wchar_t c) {
                        return static_cast<wchar_t>(std::towupper(c));
                    });
                }
            }
            RowText row = {entry.name, sub, entry.size > 0 ? FormatBytes(entry.size) : L""};
            DrawRow(row, i, y, mode_ == Mode::Browse && i == browse_cursor_.selected);
        }
        renderer_.PopClip();
    }

    void DrawRow(const RowText& row, int index, float y, bool focused) {
        const D2D1_RECT_F rect = RectF(kMargin, y, kRight, y + kRowHeight);
        const Cursor& cursor = InBrowse() ? browse_cursor_ : list_cursor_;
        if (focused) {
            renderer_.FillRounded(rect, kRowRadius, Theme::kSurfaceStrong);
        } else if (index > 0 && index - 1 != cursor.selected) {
            renderer_.FillRounded(RectF(kMargin, y - 3.0f, kRight, y - 2.0f), 0.0f,
                                  Theme::kHairline);
        }
        const float left = kMargin + kRowPadding;
        const float stat_right = kRight - kRowPadding;
        const float info_right = row.stat.empty() ? stat_right : stat_right - kStatWidth - 24.0f;
        renderer_.DrawString(row.title, Font::RowTitle,
                             RectF(left, y + 20.0f, info_right, y + 54.0f), Theme::kText,
                             HAlign::Left, VAlign::Middle);
        renderer_.DrawString(row.sub, Font::RowSub, RectF(left, y + 58.0f, info_right, y + 84.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        if (!row.stat.empty()) {
            renderer_.DrawString(row.stat, Font::Stat,
                                 RectF(stat_right - kStatWidth, y, stat_right, y + kRowHeight),
                                 Theme::kText, HAlign::Right, VAlign::Middle);
        }
        if (focused) {
            DrawRing(renderer_, rect, kRowRadius + kRingGap, 1.0f);
        }
    }

    void DrawConfirm() {
        const int index = FocusedSource();
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kConfirmTop, kSheetLeft + kSheetWidth, kConfirmTop + kConfirmHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Tr(Text::SrcRemoveTitle), Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + 100.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        if (index >= 0 && index < static_cast<int>(sources_.size())) {
            renderer_.DrawString(Widen(sources_[static_cast<std::size_t>(index)].name), Font::Meta,
                                 RectF(left, sheet.top + 108.0f, right, sheet.top + 142.0f),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        }
        renderer_.DrawString(Tr(Text::SrcRemoveBody), Font::Body,
                             RectF(left, sheet.top + 156.0f, right, sheet.top + 250.0f),
                             Theme::kTextSecondary);
        const std::wstring remove = Tr(Text::SrcRemove);
        const std::wstring keep = Tr(Text::HintCancel);
        const float remove_width = PillWidth(renderer_, remove, false);
        const float keep_width = PillWidth(renderer_, keep, false);
        const float top = sheet.top + 262.0f;
        DrawChoicePill(renderer_, RectF(left, top, left + remove_width, top + kPillHeight), remove,
                       false, confirm_focus_ == 0);
        const float keep_left = left + remove_width + kPillGap;
        DrawChoicePill(renderer_, RectF(keep_left, top, keep_left + keep_width, top + kPillHeight),
                       keep, true, confirm_focus_ == 1);
    }

    void DrawBusy(const std::wstring& text) {
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kConfirmTop, kSheetLeft + kSheetWidth, kConfirmTop + 220.0f);
        DrawSheet(renderer_, sheet);
        renderer_.DrawString(text, Font::Heading,
                             RectF(sheet.left + kSheetPadding, sheet.top, sheet.right - kSheetPadding,
                                   sheet.bottom),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    // The item being downloaded: its name, a bar, the percentage, the rate and the bytes.
    void DrawProgress() {
        const std::uint64_t done = bytes_done_.load();
        const std::uint64_t total = bytes_total_.load();
        const double fraction = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kWorkTop, kSheetLeft + kSheetWidth, kWorkTop + kWorkHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(download_name_, Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + 100.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(Tr(Text::SrcDownloading), Font::Meta,
                             RectF(left, sheet.top + 112.0f, right, sheet.top + 146.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        DrawProgressBar(renderer_, RectF(left, sheet.top + 176.0f, right, sheet.top + 192.0f),
                        std::clamp(fraction, 0.0, 1.0));
        const double rate = rate_.load();
        wchar_t speed[32];
        swprintf_s(speed, std::size(speed), L"%.1f MB/s", rate / (1024.0 * 1024.0));
        std::wstring stats = std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%   " + speed;
        if (CurrentLanguage() == Language::Portuguese) {
            std::replace(stats.begin(), stats.end(), L'.', L',');
        }
        stats += L"   " + FormatBytes(done) + L" / " + FormatBytes(total);
        renderer_.DrawString(stats, Font::MetaMono,
                             RectF(left, sheet.top + 214.0f, right, sheet.top + 254.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    std::vector<Hint> CurrentHints() const {
        switch (mode_) {
        case Mode::Download:
        case Mode::Importing:
            return {{Theme::kButtonB, L"B", Tr(Text::HintCancel)}};
        case Mode::Confirm:
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        case Mode::Browse: {
            std::vector<Hint> hints;
            if (!Loading() && failure_ == Failure::None && !entries_.empty()) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintDownload)});
            }
            if (!Loading()) {
                hints.push_back({Theme::kButtonX, L"X", Tr(Text::HintReload)});
            }
            hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
            return hints;
        }
        case Mode::Keyboard:
            break;
        case Mode::List:
            break;
        }
        std::vector<Hint> hints = {{Theme::kButtonA, L"A", Tr(Text::HintSelect)}};
        if (FocusedSource() >= 0) {
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::HintRemove)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
        return hints;
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    fs::path local_state_;

    Mode mode_ = Mode::List;
    std::vector<Source> sources_;
    Cursor list_cursor_;
    Cursor browse_cursor_;
    Tween scroll_;
    int confirm_focus_ = 1; // 0 remove, 1 keep
    std::unique_ptr<TextEntry> entry_;

    std::wstring browse_name_;
    std::string browse_url_;
    std::vector<Entry> entries_;
    std::wstring fetch_message_;
    Failure failure_ = Failure::None;
    std::shared_ptr<Task<FetchResult>> fetch_;
    int fetch_serial_ = 0;   // which fetch the screen started last
    int fetch_adopted_ = 0;  // which fetch's result it already took
    std::shared_ptr<Task<ImportResult>> import_;

    std::wstring toast_;
    Clock::time_point toast_until_{};
    bool closed_ = false;
    bool leaving_ = false;

    // The download worker. Joined by the screen, never detached: it writes into the file system.
    std::thread download_thread_;
    std::wstring download_name_;
    std::atomic<bool> cancel_{false};
    std::atomic<std::uint64_t> bytes_done_{0};
    std::atomic<std::uint64_t> bytes_total_{0};
    std::atomic<double> rate_{0.0};
    std::atomic<Outcome> outcome_{Outcome::Pending};
};

} // namespace

bool RunSourcesScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                      const std::filesystem::path& local_state) {
    try {
        SourcesScreen screen(renderer, window, input, local_state);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI sources screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI sources screen failed ") + error.what());
    }
    return false;
}

int CountSources(const std::filesystem::path& local_state) {
    return static_cast<int>(LoadSources(local_state).size());
}

} // namespace EdenXbox::Ui
