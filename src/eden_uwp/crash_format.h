// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace EdenXbox::Crash {
// Fixed storage only: no locale, CRT formatting, heap, or lazy initialization.
struct Line {
    char data[512]{};
    std::size_t size{};
    void Add(std::string_view text) noexcept {
        for (const char ch : text) {
            if (size < sizeof(data) - 2) {
                data[size++] = ch >= ' ' && ch != '\x7f' ? ch : '?';
            }
        }
    }
    void Number(std::uint64_t value, unsigned base = 10) noexcept {
        char digits[32];
        unsigned count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value % base];
            value /= base;
        } while (value);
        while (count) {
            Add(std::string_view(&digits[--count], 1));
        }
    }
    void Address(std::string_view module, std::uintptr_t address, std::uintptr_t base) noexcept {
        const auto separator = module.find_last_of("/\\");
        if (separator != std::string_view::npos) {
            module.remove_prefix(separator + 1);
        }
        if (module.empty() || !base || address < base) {
            Add("unknown+0x");
            Number(address, 16);
        } else {
            Add(module);
            Add("+0x");
            Number(address - base, 16);
        }
    }
    void Finish() noexcept {
        data[size++] = '\n';
        data[size] = '\0';
    }
};

constexpr bool FatalCode(std::uint32_t code, bool unhandled) noexcept {
    switch (code) {
    case 0xc0000005: // access violation
    case 0xc00000fd: // stack overflow
    case 0xc000001d: // illegal instruction
    case 0xc0000094: // integer divide by zero
    case 0xc0000409: // fail fast
    case 0xc0000374: // heap corruption
        return true;
    case 0xe06d7363: // MSVC C++ EH: never log handled throws
        return unhandled;
    default:
        return false;
    }
}
} // namespace EdenXbox::Crash
