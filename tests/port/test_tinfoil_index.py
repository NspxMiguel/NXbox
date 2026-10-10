# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the dependency-free Tinfoil parser on the host."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r"""

#include <cassert>
#include <string>
#include "eden_uwp/ui/tinfoil_index.h"
using namespace EdenXbox::Tinfoil;
const std::string base = "https://host/shop/index.json?token=secret";
Index parse(const std::string& body) {
    return ParseIndex(body, base);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string test = argv[1];
    if (test == "string_items") {
        const auto index = parse(R"({"files":["https://host/a.nsp","https://host/b.xci"]})");
        assert(index.status == Status::Ok && index.files.size() == 2);
        assert(index.files[0].name == "a.nsp" && index.files[1].url == "https://host/b.xci");
    } else if (test == "object_items") {
        const auto index = parse(R"({"files":[{"url":"a.nsp","size":123},null,17,{},false]})");
        assert(index.files.size() == 1 && index.files[0].size == 123);
    } else if (test == "fragment_names") {
        const auto index = parse(
            R"({"files":["https://host/x.nsp#Game%20Name%20%5B0100000000000000%5D%20%5Bv0%5D.nsp"]})");
        assert(index.files[0].url == "https://host/x.nsp");
        assert(index.files[0].name == "Game Name [0100000000000000] [v0].nsp");
        assert(index.files[0].kind == Kind::Game);
    } else if (test == "relative_urls") {
        const auto index = parse(
            R"({"files":["../games/a.nsp","/b.nsp","//cdn.host/c.nsp","./nested/../d.nsp","?file=e.nsp#E.nsp"]})");
        assert(index.files.size() == 5);
        assert(index.files[0].url == "https://host/games/a.nsp");
        assert(index.files[1].url == "https://host/b.nsp");
        assert(index.files[2].url == "https://cdn.host/c.nsp");
        assert(index.files[3].url == "https://host/shop/d.nsp");
        assert(index.files[4].url == "https://host/shop/index.json?file=e.nsp");
        assert(Resolve("https://host", "a.nsp") == "https://host/a.nsp");
        assert(Resolve(base, "#name") == base + "#name");
        assert(Resolve(base, "Game Name.nsp") == "https://host/shop/Game%20Name.nsp");
        assert(Resolve(base, "a//b/../c/") == "https://host/shop/a//c/");
    } else if (test == "directories") {
        const auto index =
            parse(R"({"directories":["child.json","../root.json",{"url":"/legacy.json"},null]})");
        assert(index.status == Status::Ok && index.directories.size() == 3);
        assert(index.directories[0] == "https://host/shop/child.json");
        assert(index.directories[1] == "https://host/root.json");
        assert(index.directories[2] == "https://host/legacy.json");
    } else if (test == "success") {
        const auto index = parse(
            R"({"success":"Welcome!","motd":"hello","referrer":null,"titledb":{"ignored":[1,true]},"files":[]})");
        assert(index.status == Status::Ok && index.success == "Welcome!");
    } else if (test == "error") {
        const auto index = parse(
            R"({"error":"Access denied","success":"Welcome","files":["a.nsp"],"directories":["child.json"]})");
        assert(index.status == Status::SourceError && index.error == "Access denied");
        assert(index.files.empty() && index.directories.empty());
        assert(parse(R"({"error":"","files":[]})").status == Status::Ok);
    } else if (test == "headers") {
        const auto index = parse(
            R"({"headers":{"Authorization":"Bearer secret","X-Key":"abc"},"locations":{"X-Location":"value"}})");
        assert(index.headers.size() == 3);
        assert(index.headers[1].first == "Authorization" &&
               index.headers[1].second == "Bearer secret");
    } else if (test == "header_arrays") {
        const auto index = parse(
            R"({"headers":[{"name":"X-One","value":"1"},{"X-Two":"2"},"X-Three: 3",["X-Four","4"],{"bad name":"skip","X-Bad":"a\r\nb"}]})");
        assert(index.headers.size() == 4);
        assert(index.headers[2] == std::make_pair(std::string("X-Three"), std::string("3")));
    } else if (test == "encrypted") {
        assert(parse("TINFOIL{\"files\":[]}").status == Status::Unsupported);
        assert(parse(std::string("\0\xFF\x80", 3)).status == Status::Unsupported);
        assert(parse("<html>Login</html>").status == Status::Unsupported);
        assert(parse("[]").status == Status::Unsupported);
        assert(parse(std::string("{\"unknown\":\"\xFF\"}")).status == Status::Unsupported);
    } else if (test == "numeric_sizes") {
        const auto index = parse(
            R"({"files":[{"url":"a","size":"12345"},{"url":"b","size":1.5e3},{"url":"c","size":18446744073709551615},{"url":"d","size":"9007199254740993"}]})");
        assert(index.files[0].size == 12345 && index.files[1].size == 1500);
        assert(index.files[2].size == UINT64_MAX && index.files[3].size == 9007199254740993ULL);
    } else if (test == "invalid_sizes") {
        const auto index = parse(
            R"({"files":[{"url":"a","size":-1},{"url":"b","size":"invalid"},{"url":"c","size":1e100},{"url":"d","size":"18446744073709551616"},{"url":"e","size":null}]})");
        assert(index.files.size() == 5);
        for (const auto& file : index.files)
            assert(file.size == 0);
    } else if (test == "malformed_json") {
        for (const auto body :
             {"{", "{} trailing", "{\"files\":[\"a\",]}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":1e}",
              "{\"a\":\"\\q\"}", "{\"a\":true false}"})
            assert(parse(body).status == Status::Unsupported);
        std::string deep(70, '[');
        deep += "0";
        deep += std::string(70, ']');
        assert(parse("{\"ignored\":" + deep + "}").status == Status::Unsupported);
    } else if (test == "empty_files") {
        for (const auto body : {"{}", "{\"files\":[]}", "{\"files\":null}", "{\"files\":{}}"}) {
            const auto index = parse(body);
            assert(index.status == Status::Ok && index.files.empty());
        }
    } else if (test == "duplicate_files") {
        const auto index = parse(R"({"files":["a.nsp","a.nsp",{"url":"a.nsp","size":5}]})");
        assert(index.files.size() == 3 && index.files[2].size == 5);
    } else if (test == "depth_cap") {
        Traversal traversal(base);
        for (std::size_t depth = 0; depth <= kMaxDepth; ++depth) {
            const auto request = traversal.Next();
            assert(request && request->depth == depth);
            Index index;
            index.status = Status::Ok;
            index.directories.push_back("https://host/level" + std::to_string(depth));
            traversal.Follow(*request, index);
        }
        assert(!traversal.Next());
    } else if (test == "count_cap") {
        Traversal traversal(base);
        const auto root = traversal.Next();
        Index index;
        index.status = Status::Ok;
        for (int i = 0; i < 100; ++i)
            index.directories.push_back("https://host/" + std::to_string(i));
        traversal.Follow(*root, index);
        std::size_t count = 1;
        while (const auto request = traversal.Next()) {
            ++count;
            traversal.Follow(*request, index);
        }
        assert(count == 64);
    } else if (test == "cycles_and_header_inheritance") {
        Traversal traversal(base);
        const auto root = traversal.Next();
        const auto index = parse(
            R"({"directories":["child.json","child.json","index.json?token=secret#same"],"headers":{"Authorization":"secret"}})");
        traversal.Follow(*root, index);
        const auto child = traversal.Next();
        assert(child && child->headers.size() == 1 && child->headers[0].second == "secret");
        assert(!traversal.Next());
        const auto sub = ParseIndex(
            R"({"directories":["grandchild.json"],"headers":{"authorization":"replacement"}})",
            child->url);
        traversal.Follow(*child, sub);
        const auto grandchild = traversal.Next();
        assert(grandchild && grandchild->headers.size() == 1 &&
               grandchild->headers[0].second == "replacement");
        assert(!traversal.Next());
        const auto error = parse(R"({"error":"stop","directories":["other.json"]})");
        traversal.Follow(*grandchild, error);
        assert(!traversal.Next());
    } else if (test == "classification") {
        assert(Classify("Game [0100000000000000] [v0].nsp") == Kind::Game);
        assert(Classify("Game [0100000000000000] [v65536].nsp") == Kind::Update);
        assert(Classify("Game [0100000000000800].nsp") == Kind::Update);
        assert(Classify("Game [0100000000000001].nsp") == Kind::Dlc);
        assert(Classify("DLC [v0]") == Kind::Dlc);
        assert(Classify("Game [v0]") == Kind::Game);
        assert(Classify("Game [v1]") == Kind::Update);
        assert(Classify("Unknown [bad] [v1234567890]") == Kind::Other);
        assert(Classify("Game [0100000000000000] [0100000000000800]") == Kind::Update);
    } else if (test == "unicode") {
        const auto index =
            parse(R"({"success":"Ol\u00e1 \ud83c\udfae","files":["a#Pok%C3%A9mon.nsp"]})");
        assert(index.success == "Olá 🎮" && index.files[0].name == "Pokémon.nsp");
        assert(parse(R"({"success":"\ud800"})").status == Status::Unsupported);
        assert(parse(R"({"success":"\udc00"})").status == Status::Unsupported);
        assert(parse("\xEF\xBB\xBF{}\n").status == Status::Ok);
    } else if (test == "entry_cap_and_invalid_urls") {
        std::string body = "{\"files\":[";
        for (int i = 0; i < 5005; ++i)
            body += (i ? "," : "") + std::string("\"a.nsp\"");
        body += "]}";
        assert(parse(body).files.size() == kMaxEntries);
        const auto index =
            parse(R"({"files":["ftp://host/a","","https://","http://host/a b","a.nsp"]})");
        assert(index.files.size() == 1);
    } else {
        assert(false);
    }
}
"""


class TinfoilIndexTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-tinfoil-")
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "driver.cpp"
        cls.binary = Path(cls.temp.name) / "driver"
        source.write_text(DRIVER)
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )


def make_test(case):
    def test(self):
        subprocess.run([str(self.binary), case], check=True, timeout=10)

    return test


for case in (
    "string_items",
    "object_items",
    "fragment_names",
    "relative_urls",
    "directories",
    "success",
    "error",
    "headers",
    "header_arrays",
    "encrypted",
    "numeric_sizes",
    "invalid_sizes",
    "malformed_json",
    "empty_files",
    "duplicate_files",
    "depth_cap",
    "count_cap",
    "cycles_and_header_inheritance",
    "classification",
    "unicode",
    "entry_cap_and_invalid_urls",
):
    setattr(TinfoilIndexTests, "test_" + case, make_test(case))


if __name__ == "__main__":
    unittest.main()
