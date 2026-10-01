// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace EdenXbox::Ui {

// One game of the library: what the shelf shows, read from the game's own package.
struct GameEntry {
    std::string title_id;       // 16 upper-case hex digits
    std::wstring name;          // from the package's control.nacp
    std::filesystem::path path; // absolute path of the .nsp or .xci
    std::uint64_t size = 0;     // bytes
    std::int64_t mtime = 0;     // last write time, in seconds
    std::filesystem::path icon; // cached JPEG in LocalState\library, empty when there is none
};

// Scans LocalState\games (.nsp and .xci, not recursive) on a worker thread, so the screen keeps
// drawing and pumping events meanwhile.
//
// A .nsz found there is converted to a .nsp next to it first (same name), which takes minutes for
// a big game, so the progress is observable (see CurrentConversion). The conversion writes to
// "<name>.nsp.partial" and renames it when it is complete, and the .nsz is deleted only after
// that. A failed or cancelled conversion leaves the .nsz alone and removes the partial file.
//
// Each game's title ID, name and icon are cached in LocalState\library\<TITLEID16>.json and .jpg
// (name, path, size, mtime), and the cache is reused as long as path, size and mtime still match,
// so later launches never reparse a package. A file that cannot be parsed is logged and skipped.
class LibraryScan {
public:
    // The .nsz being converted at the moment; `active` is false the rest of the time.
    struct Conversion {
        bool active = false;
        std::wstring name;        // the file's name without its extension
        int index = 0;            // which one, counting from 1
        int count = 0;            // how many this scan converts
        std::uint64_t done = 0;   // bytes of the .nsp written so far
        std::uint64_t total = 0;  // size of the .nsp, 0 until the converter knows it
    };

    explicit LibraryScan(std::filesystem::path local_state);
    ~LibraryScan(); // cancels the scan and waits for the worker
    LibraryScan(const LibraryScan&) = delete;
    LibraryScan& operator=(const LibraryScan&) = delete;

    bool Finished() const;
    int Done() const;  // files parsed so far
    int Total() const; // files to parse (0 until the folder has been listed and converted)
    Conversion CurrentConversion() const;
    // True once finished when no game could be read and at least one package failed for want of
    // keys: the library is empty because prod.keys is missing, not because there are no games.
    bool KeysMissing() const;

    // The games, sorted by name. Moves the result out; only valid once Finished().
    std::vector<GameEntry> Take();

private:
    void Run();
    std::vector<GameEntry> Scan();
    // Converts every .nsz of `compressed` that has no .nsp next to it yet, and adds the .nsp files
    // it makes to `packages`.
    void ConvertCompressed(const std::vector<std::filesystem::path>& compressed,
                           std::vector<std::filesystem::path>& packages);

    std::filesystem::path local_state_;
    std::atomic<int> done_{0};
    std::atomic<int> total_{0};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> keys_missing_{false};
    mutable std::mutex conversion_mutex_;
    Conversion conversion_;
    std::vector<GameEntry> games_;
    std::thread worker_;
};

} // namespace EdenXbox::Ui
