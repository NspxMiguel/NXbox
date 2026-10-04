// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "eden_uwp/ui/cheats_parse.h"

namespace {

void Put16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
}

void Put32(std::string& out, std::uint32_t value) {
    Put16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    Put16(out, static_cast<std::uint16_t>(value >> 16));
}

// A tiny zip writer: enough to build the shape of the database in memory.
class ZipBuilder {
public:
    void Add(const std::string& name, const std::string& content, bool compress) {
        std::string payload = content;
        std::uint16_t method = 0;
        if (compress && !content.empty()) {
            z_stream stream{};
            deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY);
            payload.resize(deflateBound(&stream, static_cast<uLong>(content.size())));
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
            stream.avail_in = static_cast<uInt>(content.size());
            stream.next_out = reinterpret_cast<Bytef*>(payload.data());
            stream.avail_out = static_cast<uInt>(payload.size());
            deflate(&stream, Z_FINISH);
            payload.resize(payload.size() - stream.avail_out);
            deflateEnd(&stream);
            method = 8;
        }
        const std::uint32_t crc = static_cast<std::uint32_t>(
            crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(content.data()),
                  static_cast<uInt>(content.size())));
        const std::uint32_t offset = static_cast<std::uint32_t>(data_.size());
        Put32(data_, 0x04034B50u);
        Put16(data_, 20);
        Put16(data_, 0);
        Put16(data_, method);
        Put32(data_, 0); // time and date
        Put32(data_, crc);
        Put32(data_, static_cast<std::uint32_t>(payload.size()));
        Put32(data_, static_cast<std::uint32_t>(content.size()));
        Put16(data_, static_cast<std::uint16_t>(name.size()));
        Put16(data_, 0);
        data_ += name;
        data_ += payload;

        Put32(directory_, 0x02014B50u);
        Put16(directory_, 20);
        Put16(directory_, 20);
        Put16(directory_, 0);
        Put16(directory_, method);
        Put32(directory_, 0);
        Put32(directory_, crc);
        Put32(directory_, static_cast<std::uint32_t>(payload.size()));
        Put32(directory_, static_cast<std::uint32_t>(content.size()));
        Put16(directory_, static_cast<std::uint16_t>(name.size()));
        Put16(directory_, 0);
        Put16(directory_, 0);
        Put16(directory_, 0);
        Put16(directory_, 0);
        Put32(directory_, 0);
        Put32(directory_, offset);
        directory_ += name;
        ++count_;
    }

    std::string Finish() const {
        std::string out = data_;
        const std::uint32_t directory_offset = static_cast<std::uint32_t>(out.size());
        out += directory_;
        Put32(out, 0x06054B50u);
        Put16(out, 0);
        Put16(out, 0);
        Put16(out, count_);
        Put16(out, count_);
        Put32(out, static_cast<std::uint32_t>(directory_.size()));
        Put32(out, directory_offset);
        Put16(out, 0);
        return out;
    }

private:
    std::string data_;
    std::string directory_;
    std::uint16_t count_ = 0;
};

} // namespace

int main() {
    using namespace EdenXbox::Cheats;
    int failures = 0;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ++failures;
            std::cerr << "FAILED: " << what << '\n';
        }
    };

    const std::string cheats =
        "[Moon Jump]\n80000002\n580F0000 0287E358\n640F0000 00000000 40F00000\n20000000\n\n"
        "{Master}\n580f0000 02a00000\n\n"
        "[Broken]\n1234\n\n"
        "[Moon Jump]\r\n20000000\r\n\n"
        "[Walk {fast}]\n300E0000 00000005\n";

    ZipBuilder builder;
    builder.Add("titles/", "", false);
    builder.Add("titles/0100A59012070000/", "", false);
    builder.Add("titles/0100A59012070000/cheats/56e1be633e5b9f44.txt", cheats, true);
    builder.Add("titles/0100A59012070000/cheats/AAAAAAAAAAAAAAAA.txt", "[X]\n20000000\n", false);
    builder.Add("titles/0100A59012070000/cheats/short.txt", "[X]\n20000000\n", false);
    builder.Add("titles/0100A59012070000/Some Game ver 1.0.1 by Someone.txt", "", false);
    builder.Add("titles/0100b91008780000/cheats/80fb5c02f0e4292a.txt", "[Y]\n20000000\n", true);
    builder.Add("titles/0100A59012070000/romfs/Content/a.lua", "x", false);
    const std::string zip = builder.Finish();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(zip.data());

    std::vector<ZipEntry> entries;
    std::string error;
    check(ReadZipDirectory(bytes, zip.size(), entries, error), "directory reads");
    check(entries.size() == 8, "eight entries");

    const TitleListing listing = FindTitleFiles(entries, "0100a59012070000");
    check(listing.builds.size() == 2, "two builds, the short name is skipped");
    check(listing.builds.size() == 2 && listing.builds[0].build_id == "56E1BE633E5B9F44",
          "build id is upper case");
    check(listing.notes.size() == 1 && listing.notes[0] == "Some Game ver 1.0.1 by Someone",
          "note without .txt");
    check(FindTitleFiles(entries, "0100000000000000").builds.empty(), "unknown title");
    check(FindTitleFiles(entries, "0100B91008780000").builds.size() == 1, "mixed case title");

    std::string text;
    check(ExtractZipEntry(bytes, zip.size(), entries[listing.builds[0].entry], 1 << 20, text,
                          error),
          "deflated entry extracts");
    check(text == cheats, "deflated entry content");
    check(!ExtractZipEntry(bytes, zip.size(), entries[listing.builds[0].entry], 10, text, error),
          "limit is enforced");
    std::string stored;
    check(ExtractZipEntry(bytes, zip.size(), entries[listing.builds[1].entry], 1 << 20, stored,
                          error) &&
              stored == "[X]\n20000000\n",
          "stored entry extracts");

    std::string corrupt = zip;
    const ZipEntry& first = entries[listing.builds[0].entry];
    corrupt[static_cast<std::size_t>(first.offset) + 30 + first.name.size() + 4] ^= 0xFF;
    std::vector<ZipEntry> corrupt_entries;
    check(ReadZipDirectory(reinterpret_cast<const std::uint8_t*>(corrupt.data()), corrupt.size(),
                           corrupt_entries, error),
          "corrupt zip directory still reads");
    {
        std::string ignored;
        check(!ExtractZipEntry(reinterpret_cast<const std::uint8_t*>(corrupt.data()),
                               corrupt.size(), corrupt_entries[listing.builds[0].entry], 1 << 20,
                               ignored, error),
              "corruption is caught (CRC or inflate)");
    }
    check(!ReadZipDirectory(bytes, 10, entries, error), "tiny input is not a zip");

    const ParsedCheats parsed = ParseCheatFile(cheats);
    check(parsed.blocks.size() == 4, "master + three cheats");
    check(parsed.dropped == 1, "the broken cheat is dropped");
    check(parsed.blocks[0].master && parsed.blocks[0].name == "Master", "master comes first");
    check(parsed.blocks[0].lines[0] == "580F0000 02A00000", "hex is upper-cased");
    check(parsed.blocks[1].name == "Moon Jump" && parsed.blocks[1].opcodes == 7, "opcode count");
    check(parsed.blocks[2].name == "Moon Jump (2)", "duplicate names are told apart");
    check(parsed.blocks[3].name == "Walk (fast)", "brackets in names are replaced");

    std::vector<bool> none(parsed.blocks.size(), false);
    check(BuildCheatFile(parsed.blocks, none).empty(), "nothing selected writes nothing");
    std::vector<bool> some(parsed.blocks.size(), false);
    some[3] = true;
    const std::string written = BuildCheatFile(parsed.blocks, some);
    check(written == "{Master}\n580F0000 02A00000\n\n[Walk (fast)]\n300E0000 00000005\n\n",
          "master plus the selected cheat");
    const ParsedCheats again = ParseCheatFile(written);
    check(again.blocks.size() == 2 && again.dropped == 0, "the written file parses back");

    check(ParseCheatFile("").blocks.empty(), "empty file");
    check(ParseCheatFile("\xEF\xBB\xBF[A]\n20000000\n").blocks.size() == 1, "BOM is skipped");
    std::string many = "[Big]\n";
    for (int i = 0; i < 300; ++i) {
        many += "20000000\n";
    }
    check(ParseCheatFile(many).blocks.empty(), "more opcodes than Eden holds");

    if (failures) {
        std::cerr << failures << " cheats parser checks failed\n";
    }
    return failures ? 1 : 0;
}
