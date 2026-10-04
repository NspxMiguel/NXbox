// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace EdenXbox::Ui {

// The cheats of one game, from the community database nx-cheats-db by sthetix, the same one CNX
// Updater (CostelaCNX) uses; see NOTICE.md. The database is one zip of Atmosphere cheat files,
// `<TITLE ID>/cheats/<BUILD ID>.txt`, downloaded at runtime and cached for 24 hours in
// LocalState\library\cheats. Nothing in this header draws: the screen in cheats_screen.cpp polls
// it every frame. Every network and disk call runs on worker threads of their own, so the render
// thread never blocks.
//
// What the player turns on is written to
//   LocalState\eden\load\<TITLEID>\NXboxCheats\cheats\<BUILDID>.txt
// which Eden's PatchManager loads as the mod "NXboxCheats" when the game boots. That file holds only
// the cheats that are on (plus the master code, when the cheat file has one) and is the source of
// truth; the choice is also kept in LocalState\cheats_<TITLEID>.json.

enum class CheatsPhase {
    Loading,  // reading the cache or downloading the database
    Ready,
    NoCheats, // the database has nothing for this game
    Offline,  // there was no cache and the download failed
    Failed,   // the database could not be read
};

struct CheatItem {
    std::string name; // UTF-8
    bool enabled = false;
    std::size_t opcodes = 0;
};

class CheatStore {
public:
    // `title_id` is the 16 upper-case hex digits.
    CheatStore(std::filesystem::path local_state, std::string title_id);
    ~CheatStore(); // cancels what is loading; a pending write still finishes
    CheatStore(const CheatStore&) = delete;
    CheatStore& operator=(const CheatStore&) = delete;

    CheatsPhase Phase() const;
    // The build IDs the database has cheats for, sorted. Empty until Ready.
    std::vector<std::string> BuildIds() const;
    // Names such as "Some Game ver 1.0.1 by Someone" the database keeps for the title, UTF-8.
    std::vector<std::string> Notes() const;
    // The cheats of one build ID (master code left out) and how many are on.
    std::vector<CheatItem> Cheats(std::size_t build) const;
    int EnabledCount(std::size_t build) const;
    // True while a change is not on disk yet.
    bool Writing() const;

    void Toggle(std::size_t build, std::size_t cheat);
    void SetAll(std::size_t build, bool enabled);
    // After Offline or Failed: starts over.
    void Retry();

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace EdenXbox::Ui
