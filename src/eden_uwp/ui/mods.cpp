// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden headers #undef the A/W macros of the Win32 calls they
// name their methods after.
#include <windows.h>

// wingdi.h defines GetObject as a macro for GetObjectW, which would rename JsonValue::GetObject().
#ifdef GetObject
#undef GetObject
#endif

#include "eden_uwp/ui/mods.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <fstream>
#include <functional>
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
#include <zlib.h>

#include "common/settings.h"
#include "eden_uwp/diagnostic.h"
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

constexpr const char* kApiBase = "https://gamebanana.com/apiv11";
constexpr int kPerPage = 15;
constexpr std::uint64_t kMaxArchiveBytes = 6ull << 30; // refuse to unpack more than this
constexpr int kMaxSearchDepth = 6;

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

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return text;
}

// ---- JSON access that tolerates missing and null fields ----

JsonObject ObjectAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Object) {
            return value.as<winrt::Windows::Data::Json::JsonObject>();
        }
    }
    return JsonObject{};
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

std::string StringAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::String) {
            return winrt::to_string(value.GetString());
        }
    }
    return {};
}

double NumberAt(const JsonObject& object, const wchar_t* key, double fallback) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Number) {
            return value.GetNumber();
        }
    }
    return fallback;
}

bool ParseObject(const std::string& text, JsonObject& object) {
    try {
        return JsonObject::TryParse(winrt::to_hstring(text), object);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// ---- HTTP ----

winrt::Windows::Web::Http::HttpClient MakeClient() {
    winrt::Windows::Web::Http::HttpClient client;
    // A failure to add the header is not worth failing the request for.
    void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
    return client;
}

Uri MakeUri(const std::string& url) {
    return Uri{winrt::to_hstring(url)};
}

// GETs a text body. Returns false, with a log line, on a network error or a status other than 200.
bool HttpGetText(const std::string& url, std::string& body) {
    try {
        const auto client = MakeClient();
        const auto response = client.GetAsync(MakeUri(url)).get();
        if (response.StatusCode() != winrt::Windows::Web::Http::HttpStatusCode::Ok) {
            Diagnostic(fmt::format("UI mods http {} status={}", url,
                                   static_cast<int>(response.StatusCode())));
            return false;
        }
        body = winrt::to_string(response.Content().ReadAsStringAsync().get());
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI mods http " + url + " failed " + winrt::to_string(error.message()));
        return false;
    }
}

// True when `url` answers a HEAD request with a success status (redirects are followed). Used to
// hide index entries whose file no longer exists.
bool HttpUrlLive(const std::string& url) {
    try {
        const auto client = MakeClient();
        winrt::Windows::Web::Http::HttpRequestMessage request{
            winrt::Windows::Web::Http::HttpMethod::Head(), MakeUri(url)};
        const auto response = client.SendRequestAsync(request).get();
        return response.IsSuccessStatusCode();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

bool HttpGetBytes(const std::string& url, std::vector<std::uint8_t>& bytes) {
    try {
        const auto client = MakeClient();
        const auto response = client.GetAsync(MakeUri(url)).get();
        if (response.StatusCode() != winrt::Windows::Web::Http::HttpStatusCode::Ok) {
            return false;
        }
        const auto buffer = response.Content().ReadAsBufferAsync().get();
        if (buffer.Length() == 0 || buffer.Length() > (8u << 20)) {
            return false;
        }
        bytes.assign(buffer.data(), buffer.data() + buffer.Length());
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI mods thumbnail failed " + winrt::to_string(error.message()));
        return false;
    }
}

// Streams `url` into `file`, reporting (bytes so far, total or 0). Returns an empty string on
// success, otherwise why it failed.
std::string HttpDownload(const std::string& url, const fs::path& file, std::uint64_t size_hint,
                         const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                         const std::atomic<bool>& cancel) {
    using namespace winrt::Windows::Storage::Streams;
    using winrt::Windows::Web::Http::HttpCompletionOption;
    try {
        const auto client = MakeClient();
        const auto response =
            client.GetAsync(MakeUri(url), HttpCompletionOption::ResponseHeadersRead).get();
        if (!response.IsSuccessStatusCode()) {
            return fmt::format("the download answered with status {}",
                               static_cast<int>(response.StatusCode()));
        }
        std::uint64_t total = size_hint;
        const auto length = response.Content().Headers().ContentLength();
        if (length) {
            total = length.Value();
        }
        const IInputStream stream = response.Content().ReadAsInputStreamAsync().get();
        const Buffer buffer(64 * 1024);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "cannot create the download file";
        }
        std::uint64_t done = 0;
        for (;;) {
            if (cancel.load()) {
                return "cancelled";
            }
            const IBuffer chunk =
                stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial).get();
            if (chunk.Length() == 0) {
                break;
            }
            out.write(reinterpret_cast<const char*>(chunk.data()),
                      static_cast<std::streamsize>(chunk.Length()));
            if (!out) {
                return "the disk is full or not writable";
            }
            done += chunk.Length();
            progress(done, total);
        }
        out.close();
        if (done == 0) {
            return "the download was empty";
        }
        progress(done, total > 0 ? total : done);
        return {};
    } catch (const winrt::hresult_error& error) {
        return "network: " + winrt::to_string(error.message());
    }
}

std::string UrlEncode(const std::string& text) {
    return winrt::to_string(Uri::EscapeComponent(winrt::to_hstring(text)));
}

// ---- Plain text from GameBanana's HTML ----

std::string HtmlToText(const std::string& html) {
    std::string out;
    bool in_tag = false;
    for (std::size_t i = 0; i < html.size(); ++i) {
        const char letter = html[i];
        if (in_tag) {
            if (letter == '>') {
                in_tag = false;
            }
            continue;
        }
        if (letter == '<') {
            in_tag = true;
            // Line breaks and block ends become a newline.
            const std::string rest = Lower(html.substr(i, 5));
            if (rest.rfind("<br", 0) == 0 || rest.rfind("</p", 0) == 0 ||
                rest.rfind("</li", 0) == 0 || rest.rfind("</h", 0) == 0) {
                out.push_back('\n');
            }
            continue;
        }
        if (letter == '&') {
            const std::size_t end = html.find(';', i);
            if (end != std::string::npos && end - i <= 8) {
                const std::string entity = html.substr(i, end - i + 1);
                const std::pair<const char*, const char*> known[] = {
                    {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                    {"&#39;", "'"}, {"&nbsp;", " "}};
                bool replaced = false;
                for (const auto& [from, to] : known) {
                    if (entity == from) {
                        out += to;
                        i = end;
                        replaced = true;
                        break;
                    }
                }
                if (replaced) {
                    continue;
                }
            }
        }
        out.push_back(letter);
    }
    // Collapse the blank runs and trim.
    std::string clean;
    int newlines = 0;
    for (const char letter : out) {
        if (letter == '\r') {
            continue;
        }
        if (letter == '\n') {
            if (++newlines <= 2) {
                clean.push_back('\n');
            }
            continue;
        }
        if ((letter == ' ' || letter == '\t') && (clean.empty() || clean.back() == ' ' ||
                                                  clean.back() == '\n')) {
            continue;
        }
        newlines = 0;
        clean.push_back(letter == '\t' ? ' ' : letter);
    }
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == ' ')) {
        clean.pop_back();
    }
    constexpr std::size_t kMaxText = 700;
    if (clean.size() > kMaxText) {
        std::size_t cut = kMaxText;
        // Do not cut in the middle of a UTF-8 sequence.
        while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        clean.resize(cut);
        clean += "...";
    }
    return clean;
}

// ---- Zip ----

std::uint16_t U16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct ZipEntry {
    std::string name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
};

bool ReadZipDirectory(std::ifstream& in, std::uint64_t file_size, std::vector<ZipEntry>& entries,
                      std::string& error) {
    constexpr std::uint64_t kTail = 22 + 65535;
    const std::uint64_t tail_size = std::min(file_size, kTail);
    if (tail_size < 22) {
        error = "not a zip file";
        return false;
    }
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
    in.seekg(static_cast<std::streamoff>(file_size - tail_size));
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!in) {
        error = "cannot read the zip";
        return false;
    }
    std::size_t at = tail.size() - 22 + 1;
    bool found = false;
    while (at-- > 0) {
        if (U32(&tail[at]) == 0x06054B50u) {
            found = true;
            break;
        }
    }
    if (!found) {
        error = "not a zip file";
        return false;
    }
    const std::uint8_t* eocd = &tail[at];
    const std::uint32_t count = U16(eocd + 10);
    const std::uint32_t directory_size = U32(eocd + 12);
    const std::uint32_t directory_offset = U32(eocd + 16);
    if (count == 0xFFFFu || directory_size == 0xFFFFFFFFu || directory_offset == 0xFFFFFFFFu) {
        error = "zip64 archives are not supported";
        return false;
    }
    if (static_cast<std::uint64_t>(directory_offset) + directory_size > file_size) {
        error = "the zip directory is damaged";
        return false;
    }
    std::vector<std::uint8_t> directory(directory_size);
    in.seekg(directory_offset);
    in.read(reinterpret_cast<char*>(directory.data()),
            static_cast<std::streamsize>(directory.size()));
    if (!in) {
        error = "cannot read the zip directory";
        return false;
    }
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + 46 > directory.size() || U32(&directory[pos]) != 0x02014B50u) {
            error = "the zip directory is damaged";
            return false;
        }
        const std::uint8_t* header = &directory[pos];
        ZipEntry entry;
        entry.flags = U16(header + 8);
        entry.method = U16(header + 10);
        entry.crc = U32(header + 16);
        entry.compressed = U32(header + 20);
        entry.size = U32(header + 24);
        const std::size_t name_length = U16(header + 28);
        const std::size_t extra_length = U16(header + 30);
        const std::size_t comment_length = U16(header + 32);
        entry.offset = U32(header + 42);
        if (pos + 46 + name_length + extra_length + comment_length > directory.size()) {
            error = "the zip directory is damaged";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(header + 46), name_length);
        entries.push_back(std::move(entry));
        pos += 46 + name_length + extra_length + comment_length;
    }
    return true;
}

// The relative path an entry unpacks to. Returns false when the name must be refused because it
// would leave the destination folder; `out` is empty when there is nothing to unpack.
bool SafeEntryPath(const std::string& name, fs::path& out) {
    std::string normal = name;
    std::replace(normal.begin(), normal.end(), '\\', '/');
    out.clear();
    std::size_t start = 0;
    while (start <= normal.size()) {
        std::size_t end = normal.find('/', start);
        if (end == std::string::npos) {
            end = normal.size();
        }
        std::string part = normal.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == ".." || part.find(':') != std::string::npos) {
            out.clear();
            return false;
        }
        for (char& letter : part) {
            if (letter == '<' || letter == '>' || letter == '"' || letter == '|' ||
                letter == '?' || letter == '*' || static_cast<unsigned char>(letter) < 32) {
                letter = '_';
            }
        }
        out /= fs::path(Widen(part));
    }
    return true;
}

// Unpacks one entry's data into `out_path`. Returns why it failed, or an empty string.
std::string ExtractEntry(std::ifstream& in, const ZipEntry& entry, const fs::path& out_path,
                         std::uint64_t& written_total, const std::atomic<bool>& cancel) {
    std::uint8_t local[30];
    in.clear();
    in.seekg(static_cast<std::streamoff>(entry.offset));
    in.read(reinterpret_cast<char*>(local), sizeof(local));
    if (!in || U32(local) != 0x04034B50u) {
        return "a zip entry header is damaged";
    }
    const std::uint64_t data_start = entry.offset + 30 + U16(local + 26) + U16(local + 28);
    in.seekg(static_cast<std::streamoff>(data_start));

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return "cannot create " + winrt::to_string(out_path.filename().wstring());
    }
    std::vector<std::uint8_t> input(64 * 1024);
    std::vector<std::uint8_t> output(64 * 1024);
    std::uint64_t remaining = entry.compressed;
    uLong crc = crc32(0L, Z_NULL, 0);
    std::uint64_t produced_total = 0;

    if (entry.method == 0) {
        while (remaining > 0) {
            if (cancel.load()) {
                return "cancelled";
            }
            const std::size_t chunk =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
            in.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(chunk));
            if (!in) {
                return "the zip is truncated";
            }
            crc = crc32(crc, input.data(), static_cast<uInt>(chunk));
            out.write(reinterpret_cast<const char*>(input.data()),
                      static_cast<std::streamsize>(chunk));
            remaining -= chunk;
            produced_total += chunk;
        }
    } else if (entry.method == 8) {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            return "zlib could not start";
        }
        int status = Z_OK;
        std::string failure;
        while (status != Z_STREAM_END) {
            if (cancel.load()) {
                failure = "cancelled";
                break;
            }
            if (stream.avail_in == 0) {
                const std::size_t chunk =
                    static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
                if (chunk == 0) {
                    failure = "the zip is truncated";
                    break;
                }
                in.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(chunk));
                if (!in) {
                    failure = "the zip is truncated";
                    break;
                }
                remaining -= chunk;
                stream.next_in = input.data();
                stream.avail_in = static_cast<uInt>(chunk);
            }
            stream.next_out = output.data();
            stream.avail_out = static_cast<uInt>(output.size());
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                failure = fmt::format("zlib error {}", status);
                break;
            }
            const std::size_t produced = output.size() - stream.avail_out;
            if (produced > 0) {
                crc = crc32(crc, output.data(), static_cast<uInt>(produced));
                out.write(reinterpret_cast<const char*>(output.data()),
                          static_cast<std::streamsize>(produced));
                produced_total += produced;
                if (written_total + produced_total > kMaxArchiveBytes) {
                    failure = "the archive unpacks to too much data";
                    break;
                }
            }
        }
        inflateEnd(&stream);
        if (!failure.empty()) {
            return failure;
        }
    } else {
        return fmt::format("unsupported zip compression method {}", entry.method);
    }
    out.close();
    if (!out) {
        return "the disk is full or not writable";
    }
    if (static_cast<std::uint32_t>(crc) != entry.crc) {
        return "a file of the zip is corrupt (CRC mismatch)";
    }
    written_total += produced_total;
    return {};
}

// Unpacks a .zip into `destination`. Returns why it failed, or an empty string.
std::string ExtractZip(const fs::path& zip_path, const fs::path& destination,
                       const std::atomic<bool>& cancel) {
    std::error_code size_error;
    const std::uint64_t file_size = fs::file_size(zip_path, size_error);
    if (size_error) {
        return "cannot read the zip";
    }
    std::ifstream in(zip_path, std::ios::binary);
    if (!in) {
        return "cannot open the zip";
    }
    std::vector<ZipEntry> entries;
    std::string error;
    if (!ReadZipDirectory(in, file_size, entries, error)) {
        return error;
    }
    std::uint64_t declared = 0;
    for (const ZipEntry& entry : entries) {
        declared += entry.size;
    }
    if (declared > kMaxArchiveBytes) {
        return "the archive unpacks to too much data";
    }
    std::error_code ec;
    fs::create_directories(destination, ec);
    std::uint64_t written = 0;
    for (const ZipEntry& entry : entries) {
        if (cancel.load()) {
            return "cancelled";
        }
        if (entry.flags & 1u) {
            return "the zip is password protected";
        }
        const bool is_directory = !entry.name.empty() && (entry.name.back() == '/' ||
                                                          entry.name.back() == '\\');
        fs::path relative;
        if (!SafeEntryPath(entry.name, relative)) {
            return "the zip has an unsafe path";
        }
        if (relative.empty()) {
            continue;
        }
        const fs::path target = destination / relative;
        if (is_directory) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        const std::string failure = ExtractEntry(in, entry, target, written, cancel);
        if (!failure.empty()) {
            return failure;
        }
    }
    return {};
}

// ---- Finding the mod inside an archive ----

bool IsModFolderName(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return lower == L"romfs" || lower == L"exefs" || lower == L"cheats";
}

// The shallowest folder that has a romfs, exefs or cheats folder in it (so
// atmosphere\contents\<titleid>\ is found too), searched breadth first a few levels deep.
bool FindModRoot(const fs::path& root, fs::path& found) {
    std::deque<std::pair<fs::path, int>> queue;
    queue.emplace_back(root, 0);
    while (!queue.empty()) {
        const auto [directory, depth] = queue.front();
        queue.pop_front();
        std::vector<fs::path> children;
        std::error_code ec;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_error;
            if (it->is_directory(type_error)) {
                children.push_back(it->path());
            }
        }
        std::sort(children.begin(), children.end());
        for (const fs::path& child : children) {
            if (IsModFolderName(child.filename().wstring())) {
                found = directory;
                return true;
            }
        }
        if (depth < kMaxSearchDepth) {
            for (const fs::path& child : children) {
                queue.emplace_back(child, depth + 1);
            }
        }
    }
    return false;
}

// A folder name Windows accepts, taken from the mod's name.
std::string SanitizeFolderName(const std::string& name, std::int64_t id) {
    std::string folder;
    for (const char letter : name) {
        const bool bad = letter == '<' || letter == '>' || letter == ':' || letter == '"' ||
                         letter == '/' || letter == '\\' || letter == '|' || letter == '?' ||
                         letter == '*' || static_cast<unsigned char>(letter) < 32;
        folder.push_back(bad ? '_' : letter);
    }
    while (!folder.empty() && (folder.back() == ' ' || folder.back() == '.')) {
        folder.pop_back();
    }
    while (!folder.empty() && folder.front() == ' ') {
        folder.erase(folder.begin());
    }
    constexpr std::size_t kMaxName = 48;
    if (folder.size() > kMaxName) {
        std::size_t cut = kMaxName;
        while (cut > 0 && (static_cast<unsigned char>(folder[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        folder.resize(cut);
        while (!folder.empty() && (folder.back() == ' ' || folder.back() == '.')) {
            folder.pop_back();
        }
    }
    if (folder.empty()) {
        folder = "mod-" + std::to_string(id);
    }
    return folder;
}

std::string Extension(const std::string& file_name) {
    const std::size_t dot = file_name.rfind('.');
    return dot == std::string::npos ? std::string() : Lower(file_name.substr(dot));
}

// Prefix match on the words of a category name, so "ui" does not match "build".
bool CategoryMatches(const std::string& category, const std::vector<const char*>& words) {
    const std::string lower = Lower(category);
    std::size_t start = 0;
    while (start < lower.size()) {
        while (start < lower.size() && !std::isalnum(static_cast<unsigned char>(lower[start]))) {
            ++start;
        }
        std::size_t end = start;
        while (end < lower.size() && std::isalnum(static_cast<unsigned char>(lower[end]))) {
            ++end;
        }
        const std::string token = lower.substr(start, end - start);
        for (const char* word : words) {
            if (!token.empty() && token.rfind(word, 0) == 0) {
                return true;
            }
        }
        start = end;
    }
    return false;
}

// The trademark signs are noise for the search.
std::string StripMarks(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "\xE2\x84\xA2") == 0) {
            i += 3;
            continue;
        }
        if (text.compare(i, 2, "\xC2\xAE") == 0) {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool EndsWithSwitch(const std::string& name) {
    const std::string lower = Lower(name);
    constexpr std::string_view suffix = "(switch)";
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Unofficial translations, subtitles and dubs. GameBanana files them as ordinary mods, so they are
// recognised by words in the name or the category.
bool ContainsAny(const std::string& lower, const std::vector<const char*>& words) {
    for (const char* word : words) {
        if (lower.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsTranslationEntry(const ModEntry& entry) {
    static const std::vector<const char*> words = {
        "translat", "tradu", "dub", "dublag", "subtitl", "legenda", "localiz", "locali",
        "language", "idioma", "lingua", "traduc", "voice", "voz", "audio pt", "ptbr", "pt-br"};
    return entry.category == "Translation" ||
           ContainsAny(Lower(entry.name), words) || ContainsAny(Lower(entry.category), words);
}

struct LanguageInfo {
    const char* name;
    std::vector<const char*> words;
};

const std::vector<LanguageInfo>& Languages() {
    static const std::vector<LanguageInfo> languages = {
        {"All languages", {}},
        {"Portuguese", {"portugu", "pt-br", "ptbr", "pt br", "brazil", "brasil", "[pt]", "(pt)"}},
        {"English", {"english", "[en]", "(en)", " eng "}},
        {"Spanish", {"spanish", "espa", "castellano", "[es]", "(es)"}},
        {"French", {"french", "fran", "[fr]", "(fr)"}},
        {"German", {"german", "deutsch", "[de]", "(de)"}},
        {"Italian", {"italian", "italiano", "[it]", "(it)"}},
        {"Japanese", {"japanese", "nihongo", "[jp]", "(jp)", "\xE6\x97\xA5\xE6\x9C\xAC"}},
        {"Korean", {"korean", "[ko]", "(ko)", "\xED\x95\x9C\xEA\xB5\xAD"}},
        {"Chinese", {"chinese", "mandarin", "[zh]", "(zh)", "\xE4\xB8\xAD\xE6\x96\x87"}},
    };
    return languages;
}

bool MatchesLanguage(const ModEntry& entry, int language) {
    const auto& languages = Languages();
    if (language <= 0 || language >= static_cast<int>(languages.size())) {
        return true;
    }
    // Entries that name no language at all stay in, so nothing useful disappears.
    const std::string text = " " + Lower(entry.name) + " " + Lower(entry.category) + " ";
    if (ContainsAny(text, languages[static_cast<std::size_t>(language)].words)) {
        return true;
    }
    for (std::size_t i = 1; i < languages.size(); ++i) {
        if (ContainsAny(text, languages[i].words)) {
            return false;
        }
    }
    return true;
}

struct InstalledRecord {
    std::int64_t id = 0;
    std::string name;
    std::string folder;
    std::string author;
    std::string category;
    std::string thumb_url;
    std::int64_t likes = 0;
};

struct Queue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    bool started = false;
    bool stop = false;
};

} // namespace

struct ModStore::Impl : std::enable_shared_from_this<ModStore::Impl> {
    Impl(fs::path local_state_in, std::string title_id_in, std::string game_name_in)
        : local_state(std::move(local_state_in)), title_id(std::move(title_id_in)),
          game_name(std::move(game_name_in)) {}

    fs::path local_state;
    std::string title_id;
    std::string game_name;
    std::atomic<bool> cancel{false};
    std::atomic<int> language{0};

    Queue api_queue;
    Queue thumb_queue;
    Queue install_queue;

    mutable std::mutex mutex; // guards everything below
    StorePhase phase = StorePhase::Searching;
    std::int64_t game_id = 0;
    int total = -1;
    int page = 0;
    bool more = false;
    bool page_loading = false;
    std::vector<std::shared_ptr<ModEntry>> listing;
    std::map<std::int64_t, std::shared_ptr<ModEntry>> by_id;
    std::vector<InstalledRecord> installed;
    std::set<std::string> disabled;
    std::map<std::int64_t, std::vector<std::uint8_t>> thumbs;
    std::set<std::int64_t> thumb_requested;
    std::map<std::int64_t, ModDetails> details;
    std::set<std::int64_t> details_requested;

    // ---- Threads ----

    void Post(Queue& queue, std::function<void()> task) {
        {
            const std::lock_guard<std::mutex> lock(queue.mutex);
            queue.tasks.push_back(std::move(task));
            if (!queue.started) {
                queue.started = true;
                // The thread owns a reference, so the store can go away while it finishes.
                std::thread([self = shared_from_this(), &queue] { self->Loop(queue); }).detach();
            }
        }
        queue.wake.notify_one();
    }

    void Loop(Queue& queue) {
        try {
            const ScopedApartment apartment;
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue.mutex);
                    queue.wake.wait(lock, [&] { return queue.stop || !queue.tasks.empty(); });
                    if (queue.stop) {
                        return;
                    }
                    task = std::move(queue.tasks.front());
                    queue.tasks.pop_front();
                }
                try {
                    task();
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("UI mods task failed " + winrt::to_string(error.message()));
                } catch (const std::exception& error) {
                    Diagnostic(std::string("UI mods task failed ") + error.what());
                }
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI mods worker failed " + winrt::to_string(error.message()));
        }
    }

    void StopQueue(Queue& queue) {
        {
            const std::lock_guard<std::mutex> lock(queue.mutex);
            queue.stop = true;
        }
        queue.wake.notify_all();
    }

    void Stop() {
        cancel.store(true);
        StopQueue(api_queue);
        StopQueue(thumb_queue);
        StopQueue(install_queue);
    }

    // ---- The mods.json file ----

    fs::path RecordFile() const {
        return local_state / "library" / (title_id + ".mods.json");
    }

    void LoadRecordFile() {
        try {
            std::ifstream in(RecordFile(), std::ios::binary);
            if (!in) {
                return;
            }
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            JsonObject root;
            if (!ParseObject(text, root)) {
                return;
            }
            game_id = static_cast<std::int64_t>(NumberAt(root, L"game_id", 0.0));
            for (const IJsonValue& value : ArrayAt(root, L"disabled")) {
                if (value.ValueType() == JsonValueType::String) {
                    disabled.insert(winrt::to_string(value.GetString()));
                }
            }
            for (const IJsonValue& value : ArrayAt(root, L"installed")) {
                if (value.ValueType() != JsonValueType::Object) {
                    continue;
                }
                const JsonObject item = value.as<winrt::Windows::Data::Json::JsonObject>();
                InstalledRecord record;
                record.id = static_cast<std::int64_t>(NumberAt(item, L"id", 0.0));
                record.name = StringAt(item, L"name");
                record.folder = StringAt(item, L"folder");
                record.author = StringAt(item, L"author");
                record.category = StringAt(item, L"category");
                record.thumb_url = StringAt(item, L"thumb");
                record.likes = static_cast<std::int64_t>(NumberAt(item, L"likes", 0.0));
                if (record.id != 0 && !record.folder.empty()) {
                    installed.push_back(std::move(record));
                }
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI mods record file unreadable " + winrt::to_string(error.message()));
        }
    }

    // Called with `mutex` held.
    void SaveRecordFileLocked() const {
        try {
            JsonObject root;
            root.SetNamedValue(L"game_id",
                               JsonValue::CreateNumberValue(static_cast<double>(game_id)));
            root.SetNamedValue(L"game_name",
                               JsonValue::CreateStringValue(winrt::to_hstring(game_name)));
            JsonArray off;
            for (const std::string& name : disabled) {
                off.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
            }
            root.SetNamedValue(L"disabled", off);
            JsonArray list;
            for (const InstalledRecord& record : installed) {
                JsonObject item;
                item.SetNamedValue(L"id",
                                   JsonValue::CreateNumberValue(static_cast<double>(record.id)));
                item.SetNamedValue(L"name",
                                   JsonValue::CreateStringValue(winrt::to_hstring(record.name)));
                item.SetNamedValue(
                    L"folder", JsonValue::CreateStringValue(winrt::to_hstring(record.folder)));
                item.SetNamedValue(
                    L"author", JsonValue::CreateStringValue(winrt::to_hstring(record.author)));
                item.SetNamedValue(L"category", JsonValue::CreateStringValue(
                                                    winrt::to_hstring(record.category)));
                item.SetNamedValue(
                    L"thumb", JsonValue::CreateStringValue(winrt::to_hstring(record.thumb_url)));
                item.SetNamedValue(
                    L"likes", JsonValue::CreateNumberValue(static_cast<double>(record.likes)));
                list.Append(item);
            }
            root.SetNamedValue(L"installed", list);
            std::error_code ec;
            fs::create_directories(RecordFile().parent_path(), ec);
            std::ofstream out(RecordFile(), std::ios::binary | std::ios::trunc);
            out << winrt::to_string(root.Stringify());
            if (!out) {
                Diagnostic("UI mods record file write failed");
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI mods record file write failed " + winrt::to_string(error.message()));
        }
    }

    // ---- Entries ----

    // Called with `mutex` held. One shared entry per mod id, whichever list it appears in.
    std::shared_ptr<ModEntry> EntryLocked(std::int64_t id, const std::string& name,
                                          const std::string& author, const std::string& category,
                                          const std::string& thumb_url, std::int64_t likes,
                                          std::int64_t downloads) {
        const auto found = by_id.find(id);
        if (found != by_id.end()) {
            if (downloads >= 0) {
                found->second->downloads.store(downloads);
            }
            return found->second;
        }
        auto entry = std::make_shared<ModEntry>();
        entry->id = id;
        entry->name = name;
        entry->author = author;
        entry->category = category;
        entry->thumb_url = thumb_url;
        entry->likes = likes;
        entry->downloads.store(downloads);
        for (const InstalledRecord& record : installed) {
            if (record.id == id) {
                entry->folder = record.folder;
                entry->state.store(static_cast<int>(ModState::Installed));
            }
        }
        by_id.emplace(id, entry);
        return entry;
    }

    // ---- CNX Updater content ----

    // The translations and game mods CNX Updater lists, from the public nx-links index it reads
    // (https://github.com/gamemoddesignbr/nx-links). Each entry is a zip named after a title id, so
    // the ones for this game are the files whose name starts with it. They get negative ids so they
    // never collide with GameBanana's, and they install through the same unpacking and folder rules.
    static constexpr const char* kCnxIndexUrl =
        "https://raw.githubusercontent.com/gamemoddesignbr/nx-links/master/nx-links-v204.json";
    std::map<std::int64_t, std::string> cnx_urls; // guarded by `mutex`
    std::map<std::int64_t, std::uint64_t> cnx_sizes; // guarded by `mutex`
    int cnx_count = 0;                             // guarded by `mutex`

    static std::uint64_t ParseSizeText(const std::string& text) {
        // "6.39 MB", "622 KB", "1.08 GB"
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        const std::string unit = end ? Lower(std::string(end)) : std::string();
        double scale = 1.0;
        if (unit.find("gb") != std::string::npos) {
            scale = 1024.0 * 1024.0 * 1024.0;
        } else if (unit.find("mb") != std::string::npos) {
            scale = 1024.0 * 1024.0;
        } else if (unit.find("kb") != std::string::npos) {
            scale = 1024.0;
        }
        return value > 0.0 ? static_cast<std::uint64_t>(value * scale) : 0;
    }

    void LoadCnx() {
        std::string body;
        JsonObject root;
        if (!HttpGetText(kCnxIndexUrl, body) || !ParseObject(body, root)) {
            Diagnostic("UI mods CNX index unavailable");
            return;
        }
        const std::string wanted = Lower(title_id);
        struct Found {
            std::string name;
            std::string link;
            std::string size;
            std::string category;
            std::string author;
        };
        std::vector<Found> found;
        for (const auto& [section, category] :
             {std::pair<const wchar_t*, const char*>{L"translations", "Translation"},
              std::pair<const wchar_t*, const char*>{L"modifications", "Mod"}}) {
            const JsonObject group = ObjectAt(root, section);
            for (const auto& pair : group) {
                if (pair.Value().ValueType() != JsonValueType::Object) {
                    continue;
                }
                const JsonObject item = pair.Value().GetObject();
                if (item.HasKey(L"enabled") && item.GetNamedBoolean(L"enabled", true) == false) {
                    continue;
                }
                const std::string link = StringAt(item, L"link");
                const std::size_t slash = link.rfind('/');
                const std::string file = Lower(slash == std::string::npos ? link : link.substr(slash + 1));
                if (file.rfind(wanted, 0) != 0 || Extension(file) != ".zip") {
                    continue;
                }
                // https://github.com/<owner>/<repo>/releases/... -> the owner is the credit.
                std::string owner = "CNX Updater";
                const std::string marker = "github.com/";
                const std::size_t at = link.find(marker);
                if (at != std::string::npos) {
                    const std::size_t end = link.find('/', at + marker.size());
                    if (end != std::string::npos) {
                        owner = link.substr(at + marker.size(), end - at - marker.size());
                    }
                }
                found.push_back({winrt::to_string(pair.Key()), link, StringAt(item, L"size"),
                                 category, owner});
            }
        }
        // The index is old: keep only what still downloads.
        const std::size_t listed = found.size();
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [](const Found& item) { return !HttpUrlLive(item.link); }),
                    found.end());
        if (found.empty()) {
            Diagnostic(fmt::format("UI mods CNX has nothing live for {} ({} listed)", title_id,
                                   listed));
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex);
        std::int64_t next = -1;
        std::vector<std::shared_ptr<ModEntry>> entries;
        for (const Found& item : found) {
            const std::int64_t id = next--;
            cnx_urls[id] = item.link;
            cnx_sizes[id] = ParseSizeText(item.size);
            // The index title is the game's; make the row say what it is.
            const std::string label = item.category == "Translation"
                                          ? "Translation: " + item.name
                                          : item.name;
            entries.push_back(EntryLocked(id, label, item.author, item.category, std::string(), 0, -1));
        }
        listing.insert(listing.begin(), entries.begin(), entries.end());
        cnx_count = static_cast<int>(entries.size());
        Diagnostic(fmt::format("UI mods CNX {} entries for {}", cnx_count, title_id));
    }

    // ---- Network ----

    void FindGame() {
        std::int64_t known = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            known = game_id;
            phase = known != 0 ? StorePhase::Loading : StorePhase::Searching;
        }
        if (known == 0) {
            const std::string query = StripMarks(game_name);
            std::string body;
            const std::string url = std::string(kApiBase) +
                                    "/Util/Search/Results?_sModelName=Game&_sOrder=best_match"
                                    "&_sSearchString=" +
                                    UrlEncode(query) + "&_nPage=1";
            if (!HttpGetText(url, body)) {
                SetPhase(StorePhase::Offline);
                return;
            }
            JsonObject root;
            if (!ParseObject(body, root)) {
                SetPhase(StorePhase::Offline);
                return;
            }
            std::int64_t first = 0;
            std::int64_t switch_id = 0;
            for (const IJsonValue& value : ArrayAt(root, L"_aRecords")) {
                if (value.ValueType() != JsonValueType::Object) {
                    continue;
                }
                const JsonObject record = value.as<winrt::Windows::Data::Json::JsonObject>();
                const auto id = static_cast<std::int64_t>(NumberAt(record, L"_idRow", 0.0));
                std::string name = StringAt(record, L"_sName");
                while (!name.empty() && name.back() == ' ') {
                    name.pop_back();
                }
                if (id == 0) {
                    continue;
                }
                if (first == 0) {
                    first = id;
                }
                if (switch_id == 0 && EndsWithSwitch(name)) {
                    switch_id = id;
                }
            }
            known = switch_id != 0 ? switch_id : first;
            if (known == 0) {
                Diagnostic("UI mods game not found on GameBanana: " + game_name);
                SetPhase(StorePhase::NoGame);
                return;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex);
                game_id = known;
                phase = StorePhase::Loading;
                SaveRecordFileLocked();
            }
            Diagnostic(fmt::format("UI mods game {} -> GameBanana id {}{}", title_id, known,
                                   switch_id != 0 ? " (Switch)" : ""));
        }
        LoadPage(1);
    }

    void SetPhase(StorePhase value) {
        const std::lock_guard<std::mutex> lock(mutex);
        phase = (value == StorePhase::NoGame || value == StorePhase::Offline) && cnx_count > 0
                    ? StorePhase::Ready
                    : value;
    }

    void LoadPage(int number) {
        std::int64_t id = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            id = game_id;
            page_loading = true;
        }
        const std::string url =
            fmt::format("{}/Mod/Index?_nPage={}&_nPerpage={}&_aFilters%5BGeneric_Game%5D={}"
                        "&_sSort=Generic_MostDownloaded",
                        kApiBase, number, kPerPage, id);
        std::string body;
        JsonObject root;
        if (!HttpGetText(url, body) || !ParseObject(body, root)) {
            const std::lock_guard<std::mutex> lock(mutex);
            page_loading = false;
            if (listing.empty()) {
                phase = StorePhase::Offline;
            }
            return;
        }
        const JsonObject metadata = ObjectAt(root, L"_aMetadata");
        const auto count = static_cast<int>(NumberAt(metadata, L"_nRecordCount", -1.0));
        const JsonArray records = ArrayAt(root, L"_aRecords");
        int received = 0;
        const std::lock_guard<std::mutex> lock(mutex);
        for (const IJsonValue& value : records) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject record = value.as<winrt::Windows::Data::Json::JsonObject>();
            const auto mod_id = static_cast<std::int64_t>(NumberAt(record, L"_idRow", 0.0));
            if (mod_id == 0) {
                continue;
            }
            ++received;
            std::string thumb;
            const JsonObject media = ObjectAt(record, L"_aPreviewMedia");
            const JsonArray images = ArrayAt(media, L"_aImages");
            if (images.Size() > 0 && images.GetAt(0).ValueType() == JsonValueType::Object) {
                const JsonObject image = images.GetAt(0).as<winrt::Windows::Data::Json::JsonObject>();
                std::string file = StringAt(image, L"_sFile220");
                if (file.empty()) {
                    file = StringAt(image, L"_sFile");
                }
                const std::string base = StringAt(image, L"_sBaseUrl");
                if (!file.empty() && !base.empty()) {
                    thumb = base + "/" + file;
                }
            }
            const auto entry = EntryLocked(
                mod_id, StringAt(record, L"_sName"),
                StringAt(ObjectAt(record, L"_aSubmitter"), L"_sName"),
                StringAt(ObjectAt(record, L"_aRootCategory"), L"_sName"), thumb,
                static_cast<std::int64_t>(NumberAt(record, L"_nLikeCount", 0.0)),
                static_cast<std::int64_t>(NumberAt(record, L"_nDownloadCount", -1.0)));
            const bool listed = std::any_of(listing.begin(), listing.end(),
                                            [&](const auto& other) { return other->id == mod_id; });
            if (!listed) {
                listing.push_back(entry);
            }
        }
        page = number;
        if (count >= 0) {
            total = count;
        }
        more = received >= kPerPage && (total < 0 || static_cast<int>(listing.size()) < total);
        page_loading = false;
        phase = StorePhase::Ready;
        Diagnostic(fmt::format("UI mods page {} loaded, {} mods listed", number, listing.size()));
    }

    // The files of a mod, its description and its download count.
    ModDetails FetchDetails(std::int64_t id) {
        ModDetails result;
        std::string body;
        JsonObject root;
        const std::string url =
            fmt::format("{}/Mod/{}?_csvProperties=_sName,_nDownloadCount,_aFiles,_sText", kApiBase,
                        id);
        if (!HttpGetText(url, body) || !ParseObject(body, root)) {
            result.failed = true;
            return result;
        }
        result.downloads = static_cast<std::int64_t>(NumberAt(root, L"_nDownloadCount", -1.0));
        result.description = HtmlToText(StringAt(root, L"_sText"));
        // _aFiles is an array on most mods; keep the download address next to the name.
        std::vector<std::string> urls;
        for (const IJsonValue& value : ArrayAt(root, L"_aFiles")) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject file = value.as<winrt::Windows::Data::Json::JsonObject>();
            const std::string name = StringAt(file, L"_sFile");
            const std::string address = StringAt(file, L"_sDownloadUrl");
            if (name.empty() || address.empty()) {
                continue;
            }
            result.files.emplace_back(name,
                                      static_cast<std::uint64_t>(NumberAt(file, L"_nFilesize", 0.0)));
            urls.push_back(address);
        }
        result.loaded = true;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            download_urls[id] = std::move(urls);
        }
        return result;
    }

    std::map<std::int64_t, std::vector<std::string>> download_urls; // guarded by `mutex`

    // ---- Installing ----

    void InstallMod(const std::shared_ptr<ModEntry>& entry) {
        const auto fail = [&](const std::string& why) {
            Diagnostic(fmt::format("UI mods install {} failed: {}", entry->id, why));
            entry->state.store(static_cast<int>(ModState::Failed));
        };
        entry->percent.store(0);
        entry->state.store(static_cast<int>(ModState::Downloading));

        ModDetails info;
        std::vector<std::string> addresses;
        if (entry->id < 0) {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto link = cnx_urls.find(entry->id);
            if (link != cnx_urls.end()) {
                info.loaded = true;
                info.files.emplace_back("cnx.zip", cnx_sizes[entry->id]);
                addresses.push_back(link->second);
                details[entry->id] = info;
            }
        } else {
            info = FetchDetails(entry->id);
            const std::lock_guard<std::mutex> lock(mutex);
            details[entry->id] = info;
            addresses = download_urls[entry->id];
        }
        if (!info.loaded || info.files.empty() || addresses.size() != info.files.size()) {
            fail(info.failed ? "could not read the mod's files" : "the mod has no files");
            return;
        }
        // A .zip is the one format that can be unpacked; any other file is reported as such.
        std::size_t chosen = info.files.size();
        for (std::size_t i = 0; i < info.files.size(); ++i) {
            if (Extension(info.files[i].first) == ".zip") {
                chosen = i;
                break;
            }
        }
        if (chosen == info.files.size()) {
            Diagnostic("UI mods install " + std::to_string(entry->id) +
                       " unsupported format " + info.files.front().first);
            entry->state.store(static_cast<int>(ModState::Unsupported));
            return;
        }

        const fs::path work = local_state / "mods_tmp" / std::to_string(entry->id);
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        const auto cleanup = [&] {
            std::error_code remove_error;
            fs::remove_all(work, remove_error);
        };
        const fs::path archive = work / "mod.zip";
        const std::string download_error = HttpDownload(
            addresses[chosen], archive, info.files[chosen].second,
            [&](std::uint64_t done, std::uint64_t size) {
                if (size > 0) {
                    entry->percent.store(static_cast<int>(std::min<std::uint64_t>(
                        done * 100 / size, 100)));
                }
            },
            cancel);
        if (!download_error.empty()) {
            cleanup();
            fail(download_error);
            return;
        }
        entry->percent.store(100);
        entry->state.store(static_cast<int>(ModState::Installing));

        const fs::path unpacked = work / "x";
        const std::string unzip_error = ExtractZip(archive, unpacked, cancel);
        if (!unzip_error.empty()) {
            cleanup();
            fail(unzip_error);
            return;
        }
        fs::path root;
        if (!FindModRoot(unpacked, root)) {
            cleanup();
            fail("the archive has no romfs, exefs or cheats folder");
            return;
        }
        const std::string folder = SanitizeFolderName(entry->name, entry->id);
        const fs::path destination =
            local_state / "eden" / "load" / Widen(title_id) / Widen(folder);
        fs::remove_all(destination, ec);
        fs::create_directories(destination.parent_path(), ec);
        fs::rename(root, destination, ec);
        if (ec) {
            // Another volume or a locked folder: copy instead.
            ec.clear();
            fs::copy(root, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                     ec);
            if (ec) {
                cleanup();
                fail("cannot place the mod: " + ec.message());
                return;
            }
        }
        cleanup();
        {
            const std::lock_guard<std::mutex> lock(mutex);
            entry->folder = folder;
            installed.erase(std::remove_if(installed.begin(), installed.end(),
                                           [&](const InstalledRecord& record) {
                                               return record.id == entry->id;
                                           }),
                            installed.end());
            installed.push_back({entry->id, entry->name, folder, entry->author, entry->category,
                                 entry->thumb_url, entry->likes});
            disabled.erase(folder);
            SaveRecordFileLocked();
        }
        entry->state.store(static_cast<int>(ModState::Installed));
        Diagnostic(fmt::format("UI mods install {} ok -> eden\\load\\{}\\{}", entry->id, title_id,
                               folder));
    }
};

ModStore::ModStore(fs::path local_state, std::string title_id, std::string game_name)
    : impl_(std::make_shared<Impl>(std::move(local_state), std::move(title_id),
                                   std::move(game_name))) {
    impl_->LoadRecordFile();
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl] { impl->LoadCnx(); });
    impl->Post(impl->api_queue, [impl] { impl->FindGame(); });
}

ModStore::~ModStore() {
    impl_->Stop();
}

StorePhase ModStore::Phase() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->phase;
}

int ModStore::TotalCount() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total;
}

int ModStore::InstalledCount() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return static_cast<int>(impl_->installed.size());
}

std::vector<std::shared_ptr<ModEntry>> ModStore::List(ModFilter filter) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::shared_ptr<ModEntry>> result;
    if (filter == ModFilter::Installed) {
        for (const InstalledRecord& record : impl_->installed) {
            result.push_back(impl_->EntryLocked(record.id, record.name, record.author,
                                                record.category, record.thumb_url, record.likes,
                                                -1));
        }
        return result;
    }
    static const std::vector<const char*> graphics = {
        "graphic", "texture", "skin", "visual", "shader", "resolution", "model", "effect",
        "light",   "video",   "perform", "reshade"};
    static const std::vector<const char*> interface_words = {"interface", "ui", "hud", "menu",
                                                       "font",      "button", "prompt", "icon"};
    static const std::vector<const char*> gameplay = {"gameplay", "cheat", "difficult", "balanc",
                                                      "mechanic", "tweak", "script"};
    for (const auto& entry : impl_->listing) {
        bool keep = true;
        switch (filter) {
        case ModFilter::Graphics:
            keep = CategoryMatches(entry->category, graphics);
            break;
        case ModFilter::Interface:
            keep = CategoryMatches(entry->category, interface_words);
            break;
        case ModFilter::Gameplay:
            keep = CategoryMatches(entry->category, gameplay);
            break;
        case ModFilter::Translations:
            keep = IsTranslationEntry(*entry) &&
                   MatchesLanguage(*entry, impl_->language.load());
            break;
        default:
            break;
        }
        if (keep) {
            result.push_back(entry);
        }
    }
    return result;
}

int ModStore::LanguageCount() {
    return static_cast<int>(Languages().size());
}

const char* ModStore::LanguageName(int index) {
    const auto& languages = Languages();
    return index >= 0 && index < static_cast<int>(languages.size())
               ? languages[static_cast<std::size_t>(index)].name
               : "";
}

int ModStore::Language() const {
    return impl_->language.load();
}

void ModStore::SetLanguage(int index) {
    impl_->language.store(std::clamp(index, 0, LanguageCount() - 1));
}

bool ModStore::MoreAvailable() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->more;
}

bool ModStore::PageLoading() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->page_loading;
}

void ModStore::LoadMore() {
    int next = 0;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->more || impl_->page_loading || impl_->phase != StorePhase::Ready) {
            return;
        }
        impl_->page_loading = true;
        next = impl_->page + 1;
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl, next] { impl->LoadPage(next); });
}

void ModStore::Retry() {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->phase != StorePhase::Offline) {
            return;
        }
        impl_->phase = StorePhase::Searching;
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl] { impl->FindGame(); });
}

void ModStore::RequestThumb(const std::shared_ptr<ModEntry>& entry) {
    if (entry->thumb_url.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->thumb_requested.insert(entry->id).second) {
            return;
        }
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->thumb_queue, [impl, entry] {
        if (impl->cancel.load()) {
            return;
        }
        std::vector<std::uint8_t> bytes;
        if (HttpGetBytes(entry->thumb_url, bytes)) {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            impl->thumbs[entry->id] = std::move(bytes);
        }
    });
}

bool ModStore::TakeThumb(const std::shared_ptr<ModEntry>& entry, std::vector<std::uint8_t>& bytes) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->thumbs.find(entry->id);
    if (found == impl_->thumbs.end()) {
        return false;
    }
    bytes = std::move(found->second);
    impl_->thumbs.erase(found);
    return true;
}

void ModStore::Install(const std::shared_ptr<ModEntry>& entry) {
    const ModState state = entry->State();
    if (state == ModState::Downloading || state == ModState::Installing ||
        state == ModState::Installed || state == ModState::Unsupported) {
        return;
    }
    entry->percent.store(0);
    entry->state.store(static_cast<int>(ModState::Downloading));
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->install_queue, [impl, entry] { impl->InstallMod(entry); });
}

bool ModStore::IsEnabled(const ModEntry& entry) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->disabled.count(entry.folder) == 0;
}

bool ModStore::ToggleEnabled(const std::shared_ptr<ModEntry>& entry) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (entry->State() != ModState::Installed || entry->folder.empty()) {
        return true;
    }
    bool enabled = true;
    if (impl_->disabled.erase(entry->folder) == 0) {
        impl_->disabled.insert(entry->folder);
        enabled = false;
    }
    impl_->SaveRecordFileLocked();
    Diagnostic(fmt::format("UI mods {} {}", entry->folder, enabled ? "on" : "off"));
    return enabled;
}

void ModStore::RequestDetails(const std::shared_ptr<ModEntry>& entry) {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->details_requested.insert(entry->id).second) {
            return;
        }
    }
    if (entry->id < 0) {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        ModDetails result;
        result.loaded = true;
        result.description =
            "From the public CNX Updater index (nx-links). " +
            std::string(entry->category == "Translation"
                            ? "A community translation for this game."
                            : "A community modification for this game.") +
            " Credit goes to its author and to CNX Updater.";
        const auto size = impl_->cnx_sizes.find(entry->id);
        result.files.emplace_back("cnx.zip", size != impl_->cnx_sizes.end() ? size->second : 0);
        impl_->details[entry->id] = std::move(result);
        return;
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl, entry] {
        ModDetails result = impl->FetchDetails(entry->id);
        const std::lock_guard<std::mutex> lock(impl->mutex);
        if (result.downloads >= 0) {
            entry->downloads.store(result.downloads);
        }
        if (result.failed) {
            // Allow another try the next time the sheet opens.
            impl->details_requested.erase(entry->id);
        }
        impl->details[entry->id] = std::move(result);
    });
}

ModDetails ModStore::Details(const std::shared_ptr<ModEntry>& entry) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->details.find(entry->id);
    return found != impl_->details.end() ? found->second : ModDetails{};
}

void ApplyDisabledMods(const fs::path& local_state) {
    std::error_code ec;
    fs::directory_iterator it(local_state / "library", ec);
    if (ec) {
        return;
    }
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string file = winrt::to_string(it->path().filename().wstring());
        constexpr std::string_view suffix = ".mods.json";
        if (file.size() != 16 + suffix.size() || file.compare(16, suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string title_text = file.substr(0, 16);
        char* parse_end = nullptr;
        const std::uint64_t title = std::strtoull(title_text.c_str(), &parse_end, 16);
        if (parse_end == title_text.c_str() || title == 0) {
            continue;
        }
        try {
            std::ifstream in(it->path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            JsonObject root;
            if (!ParseObject(text, root)) {
                continue;
            }
            std::vector<std::string> names;
            for (const IJsonValue& value : ArrayAt(root, L"disabled")) {
                if (value.ValueType() == JsonValueType::String) {
                    names.push_back(winrt::to_string(value.GetString()));
                }
            }
            if (!names.empty()) {
                Settings::values.disabled_addons[title] = names;
                Diagnostic(fmt::format("UI mods disabled for {}: {}", title_text, names.size()));
            }
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }
}

} // namespace EdenXbox::Ui
