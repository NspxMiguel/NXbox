// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pure part of the cheats feature: a reader for the nx-cheats-db zip (an in-memory central
// directory plus zlib inflate), the lookup of one game's files in it, and the parser and writer of
// the Atmosphere cheat text format. Nothing here touches WinRT, the disk or the network, so
// tests/port/cheats_parse.cpp builds it on the host.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

namespace EdenXbox::Cheats {

// ---- Zip ----

struct ZipEntry {
    std::string name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
};

namespace detail {

inline std::uint16_t U16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline char LowerChar(char letter) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
}

inline bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return LowerChar(x) == LowerChar(y); });
}

inline std::vector<std::string_view> SplitPath(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        if (end > start) {
            parts.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

inline bool IsHex(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char letter) {
        return std::isxdigit(static_cast<unsigned char>(letter)) != 0;
    });
}

inline std::string Upper(std::string text) {
    for (char& letter : text) {
        letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    }
    return text;
}

} // namespace detail

// Reads the central directory of a zip held in memory. Zip64 is not supported (the database is
// far below its limits). Returns false, with the reason in `error`, when it is not a usable zip.
inline bool ReadZipDirectory(const std::uint8_t* data, std::size_t size,
                             std::vector<ZipEntry>& entries, std::string& error) {
    using detail::U16;
    using detail::U32;
    entries.clear();
    if (data == nullptr || size < 22) {
        error = "not a zip file";
        return false;
    }
    constexpr std::size_t kTail = 22 + 65535;
    const std::size_t tail_start = size > kTail ? size - kTail : 0;
    std::size_t at = size - 22 + 1;
    bool found = false;
    while (at-- > tail_start) {
        if (U32(data + at) == 0x06054B50u) {
            found = true;
            break;
        }
    }
    if (!found) {
        error = "not a zip file";
        return false;
    }
    const std::uint8_t* eocd = data + at;
    const std::uint32_t count = U16(eocd + 10);
    const std::uint32_t directory_size = U32(eocd + 12);
    const std::uint32_t directory_offset = U32(eocd + 16);
    if (count == 0xFFFFu || directory_size == 0xFFFFFFFFu || directory_offset == 0xFFFFFFFFu) {
        error = "zip64 archives are not supported";
        return false;
    }
    if (static_cast<std::uint64_t>(directory_offset) + directory_size > size) {
        error = "the zip directory is damaged";
        return false;
    }
    const std::uint8_t* directory = data + directory_offset;
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + 46 > directory_size || U32(directory + pos) != 0x02014B50u) {
            error = "the zip directory is damaged";
            return false;
        }
        const std::uint8_t* header = directory + pos;
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
        if (pos + 46 + name_length + extra_length + comment_length > directory_size) {
            error = "the zip directory is damaged";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(header + 46), name_length);
        entries.push_back(std::move(entry));
        pos += 46 + name_length + extra_length + comment_length;
    }
    return true;
}

// Unpacks one entry (stored or deflated) into `out`, refusing more than `limit` bytes and
// checking the CRC. Returns false, with the reason in `error`, on any problem.
inline bool ExtractZipEntry(const std::uint8_t* data, std::size_t size, const ZipEntry& entry,
                            std::size_t limit, std::string& out, std::string& error) {
    using detail::U16;
    using detail::U32;
    out.clear();
    if ((entry.flags & 1u) != 0) {
        error = "the zip entry is encrypted";
        return false;
    }
    if (entry.size > limit) {
        error = "the zip entry is too large";
        return false;
    }
    if (entry.offset + 30 > size || U32(data + entry.offset) != 0x04034B50u) {
        error = "a zip entry header is damaged";
        return false;
    }
    const std::uint8_t* local = data + entry.offset;
    const std::uint64_t data_start = entry.offset + 30 + U16(local + 26) + U16(local + 28);
    if (data_start + entry.compressed > size) {
        error = "the zip is truncated";
        return false;
    }
    const std::uint8_t* source = data + data_start;
    if (entry.method == 0) {
        if (entry.compressed != entry.size) {
            error = "the zip entry is damaged";
            return false;
        }
        out.assign(reinterpret_cast<const char*>(source), static_cast<std::size_t>(entry.size));
    } else if (entry.method == 8) {
        out.resize(static_cast<std::size_t>(entry.size));
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            error = "zlib could not start";
            return false;
        }
        stream.next_in = const_cast<Bytef*>(source);
        stream.avail_in = static_cast<uInt>(entry.compressed);
        stream.next_out = reinterpret_cast<Bytef*>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());
        // An empty output has no byte to write into; zlib still needs the end of the stream.
        Bytef spare = 0;
        if (out.empty()) {
            stream.next_out = &spare;
            stream.avail_out = 1;
        }
        const int status = inflate(&stream, Z_FINISH);
        const std::uint64_t produced = out.empty() ? 0 : out.size() - stream.avail_out;
        inflateEnd(&stream);
        if (status != Z_STREAM_END || produced != entry.size) {
            out.clear();
            error = "zlib could not unpack the entry";
            return false;
        }
    } else {
        error = "unsupported zip compression method";
        return false;
    }
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(out.data()),
                            static_cast<uInt>(out.size()));
    if (static_cast<std::uint32_t>(crc) != entry.crc) {
        out.clear();
        error = "a file of the zip is corrupt (CRC mismatch)";
        return false;
    }
    return true;
}

// ---- One game's files in the database ----

// The database keeps `<root>/<TITLE ID>/cheats/<BUILD ID>.txt` (the root is `titles` or
// `contents`, the hex digits come in either case) and, next to the `cheats` folder, empty files
// named after the game versions and authors the cheats were made for.
struct BuildFile {
    std::string build_id;  // the first 16 hex digits, upper case: the name Eden looks for
    std::size_t entry = 0; // index into the zip directory
};

struct TitleListing {
    std::vector<BuildFile> builds; // sorted by build id, one per id
    std::vector<std::string> notes; // "Game ver 1.0.0 by Someone", UTF-8, may be empty
};

inline TitleListing FindTitleFiles(const std::vector<ZipEntry>& entries,
                                   std::string_view title_id) {
    TitleListing listing;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& entry = entries[i];
        if (entry.name.empty() || entry.name.back() == '/') {
            continue;
        }
        const std::vector<std::string_view> parts = detail::SplitPath(entry.name);
        if (parts.size() == 4 && detail::EqualsNoCase(parts[1], title_id) &&
            detail::EqualsNoCase(parts[2], "cheats") && parts[3].size() >= 4 + 16 &&
            detail::EqualsNoCase(parts[3].substr(parts[3].size() - 4), ".txt")) {
            const std::string_view stem = parts[3].substr(0, parts[3].size() - 4);
            if (!detail::IsHex(stem)) {
                continue;
            }
            std::string build_id = detail::Upper(std::string(stem.substr(0, 16)));
            const bool known = std::any_of(listing.builds.begin(), listing.builds.end(),
                                           [&](const BuildFile& file) {
                                               return file.build_id == build_id;
                                           });
            if (!known) {
                listing.builds.push_back({std::move(build_id), i});
            }
        } else if (parts.size() == 3 && detail::EqualsNoCase(parts[1], title_id) &&
                   entry.size == 0 && parts[2].size() > 4 &&
                   detail::EqualsNoCase(parts[2].substr(parts[2].size() - 4), ".txt")) {
            listing.notes.emplace_back(parts[2].substr(0, parts[2].size() - 4));
        }
    }
    std::sort(listing.builds.begin(), listing.builds.end(),
              [](const BuildFile& a, const BuildFile& b) { return a.build_id < b.build_id; });
    return listing;
}

// ---- The cheat text format ----

// Eden's parser (core/memory/cheat_engine.cpp) rejects the WHOLE file at the first thing it does
// not understand and holds 0x100 opcodes per cheat, so every block is validated here and a bad one
// is dropped instead of poisoning the rest.
inline constexpr std::size_t kMaxOpcodesPerCheat = 0x100;
inline constexpr std::size_t kMaxCheatNameBytes = 63; // Eden keeps 0x40 bytes with the terminator

struct CheatBlock {
    std::string name;               // UTF-8, free of the bracket characters
    bool master = false;            // `{Name}`: runs whenever the file is loaded
    std::vector<std::string> lines; // opcode lines: 8-digit upper-case hex words, space separated
    std::size_t opcodes = 0;
};

struct ParsedCheats {
    std::vector<CheatBlock> blocks; // valid blocks in file order; at most one master, first
    int dropped = 0;                // blocks left out because they were damaged or too long
};

namespace detail {

inline std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// A name Eden's parser reads back whole: no brackets or braces, no control characters, at most
// kMaxCheatNameBytes bytes without cutting a UTF-8 sequence.
inline std::string CleanName(std::string_view raw) {
    std::string name;
    bool last_space = true;
    for (const char letter : raw) {
        char mapped = letter;
        if (letter == '[' || letter == '{') {
            mapped = '(';
        } else if (letter == ']' || letter == '}') {
            mapped = ')';
        } else if (static_cast<unsigned char>(letter) < 32 || letter == 127) {
            mapped = ' ';
        }
        if (mapped == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        name.push_back(mapped);
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    if (name.size() > kMaxCheatNameBytes) {
        std::size_t cut = kMaxCheatNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        name.resize(cut);
        while (!name.empty() && name.back() == ' ') {
            name.pop_back();
        }
    }
    return name;
}

// One opcode line to its canonical form, or empty when a word is not exactly eight hex digits.
inline std::string CleanCodeLine(std::string_view line, std::size_t& words) {
    std::string out;
    words = 0;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (pos >= line.size()) {
            break;
        }
        std::size_t end = pos;
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        const std::string_view word = line.substr(pos, end - pos);
        if (word.size() != 8 || !IsHex(word)) {
            words = 0;
            return {};
        }
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += Upper(std::string(word));
        ++words;
        pos = end;
    }
    return out;
}

} // namespace detail

// Parses an Atmosphere cheat file: `[Name]` starts a cheat, `{Name}` the master code, and the
// lines below are opcode words. Lines before the first header and `//` or `#` comments are
// ignored. Cheats with the same name get " (2)", " (3)"... so every one can be told apart.
inline ParsedCheats ParseCheatFile(std::string_view text) {
    ParsedCheats result;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    CheatBlock current;
    bool open = false;
    bool damaged = false;
    bool master_seen = false;

    const auto close_block = [&] {
        if (!open) {
            return;
        }
        open = false;
        if (damaged || current.name.empty() || current.opcodes == 0 ||
            current.opcodes > kMaxOpcodesPerCheat) {
            ++result.dropped;
        } else if (current.master && master_seen) {
            // Eden accepts one master block per file; a second one is left out.
            ++result.dropped;
        } else {
            if (current.master) {
                master_seen = true;
                result.blocks.insert(result.blocks.begin(), current);
            } else {
                result.blocks.push_back(current);
            }
        }
        current = CheatBlock{};
        damaged = false;
    };

    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = detail::Trim(text.substr(pos, end - pos));
        pos = end + 1;
        // Some files carry a byte order mark in front of a header in the middle of the file.
        while (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            line = detail::Trim(line.substr(3));
        }
        if (line.empty() || line.rfind("//", 0) == 0 || line[0] == '#') {
            continue;
        }
        if (line[0] == '[' || line[0] == '{') {
            close_block();
            const char closer = line[0] == '[' ? ']' : '}';
            const std::size_t close_at = line.find(closer);
            open = true;
            current.master = line[0] == '{';
            if (close_at == std::string_view::npos) {
                damaged = true;
            } else {
                current.name = detail::CleanName(line.substr(1, close_at - 1));
            }
            continue;
        }
        if (!open) {
            continue;
        }
        std::size_t words = 0;
        const std::string clean = detail::CleanCodeLine(line, words);
        if (clean.empty()) {
            damaged = true;
            continue;
        }
        current.lines.push_back(clean);
        current.opcodes += words;
    }
    close_block();

    // Tell equal names apart.
    for (std::size_t i = 0; i < result.blocks.size(); ++i) {
        int seen = 1;
        for (std::size_t j = i + 1; j < result.blocks.size(); ++j) {
            if (result.blocks[j].name == result.blocks[i].name) {
                result.blocks[j].name = detail::CleanName(result.blocks[i].name + " (" +
                                                           std::to_string(++seen) + ")");
            }
        }
    }
    return result;
}

// The file Eden loads: the master block (when there is one) and every cheat whose flag is set.
// Returns an empty string when no cheat is selected, because the master code alone does nothing.
inline std::string BuildCheatFile(const std::vector<CheatBlock>& blocks,
                                  const std::vector<bool>& enabled) {
    std::string out;
    bool any = false;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (!blocks[i].master && i < enabled.size() && enabled[i]) {
            any = true;
            break;
        }
    }
    if (!any) {
        return out;
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const CheatBlock& block = blocks[i];
        if (!block.master && !(i < enabled.size() && enabled[i])) {
            continue;
        }
        out += block.master ? '{' : '[';
        out += block.name;
        out += block.master ? "}\n" : "]\n";
        for (const std::string& line : block.lines) {
            out += line;
            out += '\n';
        }
        out += '\n';
    }
    return out;
}

} // namespace EdenXbox::Cheats
