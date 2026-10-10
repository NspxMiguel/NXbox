// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure Tinfoil index parsing and traversal scheduling. Network requests belong to the caller.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdenXbox::Tinfoil {
inline constexpr std::size_t kMaxEntries = 5000;
inline constexpr std::size_t kMaxDepth = 3;    // root is depth zero
inline constexpr std::size_t kMaxIndexes = 64; // includes the root and failed requests

enum class Kind { Other, Game, Update, Dlc };
enum class Status { Ok, Unsupported, SourceError };
using Headers = std::vector<std::pair<std::string, std::string>>;
struct File {
    std::string name;
    std::string url; // absolute, without the display-name fragment
    std::uint64_t size = 0;
    Kind kind = Kind::Other;
};
struct Index {
    Status status = Status::Unsupported;
    std::vector<File> files;
    std::vector<std::string> directories;
    Headers headers;
    std::string success;
    std::string error;
};

namespace detail {
inline std::string Trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
inline std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}
inline int HexValue(char c) {
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

inline std::string UrlDecode(const std::string& text) {
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

inline bool IsHttpUrl(const std::string& url) {
    const auto lower = Lower(url.substr(0, 8));
    const std::size_t start = lower.rfind("https://", 0) == 0  ? 8
                              : lower.rfind("http://", 0) == 0 ? 7
                                                               : 0;
    return start != 0 && url.size() > start && url[start] != '/' && url[start] != '?' &&
           url[start] != '#' && std::none_of(url.begin(), url.end(), [](unsigned char c) {
               return c <= ' ' || c == 127;
           });
}

// Small strict JSON reader. Unknown fields are still validated; nesting is bounded.
struct Value {
    enum class Type { Null, String, Number, Object, Array, Boolean } type = Type::Null;
    std::string text;
    std::vector<std::pair<std::string, Value>> object;
    std::vector<Value> array;
    const Value& At(std::string_view key) const {
        for (auto it = object.rbegin(); it != object.rend(); ++it)
            if (it->first == key)
                return it->second;
        static const Value empty;
        return empty;
    }
    std::string String() const {
        return type == Type::String ? text : std::string{};
    }
};
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}
    bool Read(Value& value) {
        return ReadValue(value, 0) && (Space(), pos_ == text_.size());
    }

private:
    void Space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\r' || text_[pos_] == '\n'))
            ++pos_;
    }
    bool Take(char c) {
        Space();
        if (pos_ == text_.size() || text_[pos_] != c)
            return false;
        ++pos_;
        return true;
    }
    bool Hex(unsigned& code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == text_.size())
                return false;
            const int digit = HexValue(text_[pos_++]);
            if (digit < 0)
                return false;
            code = code * 16 + static_cast<unsigned>(digit);
        }
        return true;
    }
    static void Utf8(std::string& out, unsigned code) {
        if (code <= 0x7F)
            out.push_back(static_cast<char>(code));
        else {
            if (code > 0xFFFF)
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            else if (code > 0x7FF)
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            else
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            if (code > 0xFFFF)
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
            if (code > 0x7FF)
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (code & 63)));
        }
    }
    bool String(std::string& out) {
        if (!Take('"'))
            return false;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"')
                return true;
            if (c < 32)
                return false;
            if (c != '\\') {
                if (c < 128) {
                    out.push_back(static_cast<char>(c));
                    continue;
                }
                const unsigned remaining = c >= 0xC2 && c <= 0xDF   ? 1
                                           : c >= 0xE0 && c <= 0xEF ? 2
                                           : c >= 0xF0 && c <= 0xF4 ? 3
                                                                    : 0;
                if (remaining == 0 || pos_ + remaining > text_.size())
                    return false;
                unsigned code = c & (remaining == 1 ? 31 : remaining == 2 ? 15 : 7);
                for (unsigned i = 0; i < remaining; ++i) {
                    const auto byte = static_cast<unsigned char>(text_[pos_++]);
                    if ((byte & 0xC0) != 0x80)
                        return false;
                    code = (code << 6) | (byte & 63);
                }
                if ((remaining == 2 && code < 0x800) || (remaining == 3 && code < 0x10000) ||
                    code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                    return false;
                Utf8(out, code);
                continue;
            }
            if (pos_ == text_.size())
                return false;
            const char escape = text_[pos_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned code;
                if (!Hex(code))
                    return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (pos_ + 2 > text_.size() || text_.substr(pos_, 2) != "\\u")
                        return false;
                    pos_ += 2;
                    unsigned low;
                    if (!Hex(low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                } else if (code >= 0xDC00 && code <= 0xDFFF)
                    return false;
                Utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool Digits() {
        const auto start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
            ++pos_;
        return pos_ > start;
    }
    bool ReadValue(Value& value, unsigned depth) {
        Space();
        if (depth > 64 || pos_ == text_.size())
            return false;
        const char c = text_[pos_];
        if (c == '"') {
            value.type = Value::Type::String;
            return String(value.text);
        }
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            value.type = object ? Value::Type::Object : Value::Type::Array;
            ++pos_;
            if (Take(object ? '}' : ']'))
                return true;
            do {
                std::string key;
                if (object && (!String(key) || !Take(':')))
                    return false;
                Value child;
                if (!ReadValue(child, depth + 1))
                    return false;
                if (object)
                    value.object.emplace_back(std::move(key), std::move(child));
                else
                    value.array.push_back(std::move(child));
                if (Take(object ? '}' : ']'))
                    return true;
            } while (Take(','));
            return false;
        }
        for (const auto literal : {"null", "true", "false"}) {
            const std::string_view word(literal);
            if (text_.substr(pos_, word.size()) == word) {
                pos_ += word.size();
                value.type = word == "null" ? Value::Type::Null : Value::Type::Boolean;
                return true;
            }
        }
        const auto start = pos_;
        if (c == '-')
            ++pos_;
        if (pos_ == text_.size())
            return false;
        if (text_[pos_] == '0')
            ++pos_;
        else if (text_[pos_] < '1' || text_[pos_] > '9' || !Digits())
            return false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!Digits())
                return false;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (!Digits())
                return false;
        }
        value.type = Value::Type::Number;
        value.text = std::string(text_.substr(start, pos_ - start));
        return true;
    }
    std::string_view text_;
    std::size_t pos_ = 0;
};

inline std::uint64_t Size(const Value& value) {
    if (value.type != Value::Type::Number && value.type != Value::Type::String)
        return 0;
    const std::string text = Trim(value.text);
    if (text.empty())
        return 0;
    // Preserve all 64 bits for integer sizes, including numbers above double precision.
    if (std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        std::uint64_t size = 0;
        for (char c : text) {
            const auto digit = static_cast<unsigned>(c - '0');
            if (size > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
                return 0;
            size = size * 10 + digit;
        }
        return size;
    }
    // Numeric strings retain the previous atof-style prefix parsing.
    const long double size = std::strtold(text.c_str(), nullptr);
    return std::isfinite(size) && size > 0 && size < std::ldexp(1.0L, 64)
               ? static_cast<std::uint64_t>(size)
               : 0;
}
inline void AddHeader(Headers& headers, const std::string& name, const std::string& value) {
    // Refuse invalid field names and line breaks before passing data to an HTTP API.
    if (name.empty() ||
        std::any_of(name.begin(), name.end(),
                    [](unsigned char c) {
                        return !(std::isalnum(c) ||
                                 std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) !=
                                     std::string_view::npos);
                    }) ||
        value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos)
        return;
    headers.emplace_back(name, value);
}
inline void ReadHeaders(const Value& value, Headers& headers) {
    if (value.type == Value::Type::Object) {
        if (value.At("name").type == Value::Type::String &&
            value.At("value").type == Value::Type::String)
            AddHeader(headers, value.At("name").text, value.At("value").text);
        else
            for (const auto& item : value.object)
                if (item.second.type == Value::Type::String)
                    AddHeader(headers, item.first, item.second.text);
    } else if (value.type == Value::Type::Array) {
        for (const auto& item : value.array) {
            if (item.type == Value::Type::String) {
                const auto colon = item.text.find(':');
                if (colon != std::string::npos)
                    AddHeader(headers, Trim(item.text.substr(0, colon)),
                              Trim(item.text.substr(colon + 1)));
            } else if (item.type == Value::Type::Array && item.array.size() == 2 &&
                       item.array[0].type == Value::Type::String &&
                       item.array[1].type == Value::Type::String)
                AddHeader(headers, item.array[0].text, item.array[1].text);
            else
                ReadHeaders(item, headers);
        }
    }
}
} // namespace detail

// Resolves HTTP references, including root-relative paths, queries, and dot segments.
inline std::optional<std::string> Resolve(const std::string& base, const std::string& reference) {
    std::string ref = detail::Trim(reference);
    if (ref.empty())
        return std::nullopt;
    if (detail::IsHttpUrl(ref))
        return ref;
    if (!detail::IsHttpUrl(base))
        return std::nullopt;
    // WinRT Uri encoded spaces and UTF-8 bytes in relative references.
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : ref) {
        if (c == ' ' || c >= 128) {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        } else
            encoded += static_cast<char>(c);
    }
    ref = std::move(encoded);
    const auto scheme = base.find("://");
    if (ref.rfind("//", 0) == 0) {
        const auto absolute = base.substr(0, scheme + 1) + ref;
        return detail::IsHttpUrl(absolute) ? std::optional<std::string>(absolute) : std::nullopt;
    }
    if (ref.substr(0, ref.find_first_of("/?#")).find(':') != std::string::npos)
        return std::nullopt;
    const auto authority_end = base.find_first_of("/?#", scheme + 3);
    const std::string origin = base.substr(0, authority_end);
    const auto path_end =
        base.find_first_of("?#", authority_end == std::string::npos ? base.size() : authority_end);
    const std::string base_path = authority_end != std::string::npos && base[authority_end] == '/'
                                      ? base.substr(authority_end, path_end - authority_end)
                                      : "/";
    std::string target;
    if (ref[0] == '#')
        target = base.substr(0, base.find('#')) + ref;
    else if (ref[0] == '?')
        target = origin + base_path + ref;
    else {
        const auto suffix = ref.find_first_of("?#");
        const auto path = ref.substr(0, suffix);
        std::string merged =
            path[0] == '/' ? path : base_path.substr(0, base_path.find_last_of('/') + 1) + path;
        // RFC 3986 dot-segment removal, preserving empty segments and trailing slashes.
        std::string normalized;
        while (!merged.empty()) {
            if (merged.rfind("../", 0) == 0)
                merged.erase(0, 3);
            else if (merged.rfind("./", 0) == 0)
                merged.erase(0, 2);
            else if (merged.rfind("/./", 0) == 0)
                merged.erase(0, 2);
            else if (merged == "/.")
                merged = "/";
            else if (merged.rfind("/../", 0) == 0 || merged == "/..") {
                merged.erase(0, 3);
                normalized.erase(normalized.find_last_of('/') == std::string::npos
                                     ? 0
                                     : normalized.find_last_of('/'));
                if (merged.empty())
                    merged = "/";
            } else if (merged == "." || merged == "..")
                merged.clear();
            else {
                const auto end = merged.find('/', merged[0] == '/' ? 1 : 0);
                normalized += merged.substr(0, end);
                merged.erase(0, end == std::string::npos ? merged.size() : end);
            }
        }
        target = origin + normalized + (suffix == std::string::npos ? "" : ref.substr(suffix));
    }
    return detail::IsHttpUrl(target) ? std::optional<std::string>(target) : std::nullopt;
}

// Keep the screen's existing title-id/version classification behavior.
inline Kind Classify(const std::string& name) {
    const std::string lower = detail::Lower(name);
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
        if (tag.size() == 16 && all(0, [](char c) { return detail::HexValue(c) >= 0; })) {
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

inline Index ParseIndex(std::string_view body, const std::string& base) {
    Index index;
    if (body.substr(0, 3) == "\xEF\xBB\xBF")
        body.remove_prefix(3);
    detail::Value root;
    if (!detail::JsonReader(body).Read(root) || root.type != detail::Value::Type::Object)
        return index;
    index.status = Status::Ok;
    index.success = root.At("success").String();
    index.error = root.At("error").String();
    if (!index.error.empty()) {
        index.status = Status::SourceError;
        return index;
    }
    detail::ReadHeaders(root.At("locations"), index.headers);
    detail::ReadHeaders(root.At("headers"), index.headers);
    for (const auto& item : root.At("files").array) {
        if (index.files.size() >= kMaxEntries)
            break;
        const auto reference =
            item.type == detail::Value::Type::String ? item.text : item.At("url").String();
        const auto absolute = Resolve(base, reference);
        if (!absolute)
            continue;
        File file;
        const auto hash = absolute->find('#');
        file.url = absolute->substr(0, hash);
        if (hash != std::string::npos)
            file.name = detail::UrlDecode(absolute->substr(hash + 1));
        if (file.name.empty()) {
            const auto path = file.url.substr(0, file.url.find('?'));
            const auto last = path.find_last_of('/');
            if (last != std::string::npos && last > path.find("://") + 2)
                file.name = detail::UrlDecode(path.substr(last + 1));
        }
        if (file.name.empty()) {
            const auto start = file.url.find("://") + 3;
            file.name = file.url.substr(start, file.url.find_first_of("/?#", start) - start);
            const auto at = file.name.rfind('@');
            if (at != std::string::npos)
                file.name.erase(0, at + 1);
        }
        file.kind = Classify(file.name);
        file.size = detail::Size(item.At("size"));
        index.files.push_back(std::move(file)); // keep duplicate files, as the screen did
    }
    for (const auto& item : root.At("directories").array) {
        const auto reference =
            item.type == detail::Value::Type::String ? item.text : item.At("url").String();
        if (auto absolute = Resolve(base, reference)) {
            absolute->erase(absolute->find('#') == std::string::npos ? absolute->size()
                                                                     : absolute->find('#'));
            index.directories.push_back(std::move(*absolute));
        }
    }
    return index;
}

struct Request {
    std::string url;
    std::size_t depth = 0;
    Headers headers;
};
// A bounded breadth-first queue shared by host tests and the UI. It performs no I/O.
class Traversal {
public:
    explicit Traversal(std::string url) {
        url.erase(url.find('#') == std::string::npos ? url.size() : url.find('#'));
        visited_.insert(url);
        requests_.push_back({std::move(url), 0, {}});
    }
    std::optional<Request> Next() {
        if (next_ == requests_.size())
            return std::nullopt;
        return requests_[next_++];
    }
    void Follow(const Request& parent, const Index& index) {
        if (index.status != Status::Ok || parent.depth >= kMaxDepth)
            return;
        Headers headers = parent.headers;
        for (const auto& header : index.headers) {
            headers.erase(std::remove_if(headers.begin(), headers.end(),
                                         [&](const auto& old) {
                                             return detail::Lower(old.first) ==
                                                    detail::Lower(header.first);
                                         }),
                          headers.end());
            headers.push_back(header);
        }
        for (const auto& directory : index.directories) {
            if (requests_.size() >= kMaxIndexes)
                break;
            if (visited_.insert(directory).second)
                requests_.push_back({directory, parent.depth + 1, headers});
        }
    }

private:
    std::set<std::string> visited_;
    std::vector<Request> requests_;
    std::size_t next_ = 0;
};
} // namespace EdenXbox::Tinfoil
