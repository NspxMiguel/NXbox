// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace EdenXbox::Ui {

// The mod store of one game, backed by GameBanana (docs/nxbox-ui.md, Increment 2). Nothing in this
// header draws: the screen in mods_screen.cpp polls it every frame. Every network and disk call
// runs on worker threads of its own (a WinRT HttpClient, as in shader_share.cpp), so the render
// thread never blocks.

enum class ModState : int {
    NotInstalled,
    Downloading,
    Installing,
    Installed,
    Unsupported, // the only file is a .7z or .rar
    Failed,
};

// One mod of the list. The text fields never change after the entry is shared; the state and the
// numbers are atomics because the workers update them while the screen reads them.
struct ModEntry {
    std::int64_t id = 0;
    std::string name; // UTF-8
    std::string author;
    std::string category;
    std::string thumb_url;
    std::int64_t likes = 0;
    std::atomic<std::int64_t> downloads{-1}; // -1 while the listing does not carry it
    std::atomic<int> state{static_cast<int>(ModState::NotInstalled)};
    std::atomic<int> percent{0};
    std::string folder; // the folder under eden\load\<TITLEID>\ once installed

    ModState State() const {
        return static_cast<ModState>(state.load());
    }
};

// What "Y, view the mod" shows.
struct ModDetails {
    bool loaded = false;
    bool failed = false;
    std::int64_t downloads = -1;
    std::string description; // plain text
    std::vector<std::pair<std::string, std::uint64_t>> files; // name and size in bytes
};

enum class StorePhase {
    Searching, // looking for the game on GameBanana
    Loading,   // the game is known, the first page is on its way
    Ready,
    NoGame,  // GameBanana has no such game
    Offline, // the network failed before there was anything to show
};

enum class ModFilter { Top, Graphics, Interface, Gameplay, Translations, Installed };

class ModStore {
public:
    // `title_id` is the 16 upper-case hex digits; `game_name` is what the library shows.
    ModStore(std::filesystem::path local_state, std::string title_id, std::string game_name);
    ~ModStore(); // cancels what is running; a download in flight stops at its next chunk
    ModStore(const ModStore&) = delete;
    ModStore& operator=(const ModStore&) = delete;

    StorePhase Phase() const;
    // How many mods GameBanana lists for the game, -1 while unknown.
    int TotalCount() const;
    int InstalledCount() const;
    // The mods of a chip. The categories filter what is loaded so far (GameBanana's categories
    // differ from game to game, so they match by keyword); Installed comes from disk.
    std::vector<std::shared_ptr<ModEntry>> List(ModFilter filter) const;
    bool MoreAvailable() const;
    bool PageLoading() const;
    // Loads the next page; does nothing while one is loading or when there are no more.
    void LoadMore();
    // After Offline: starts over.
    void Retry();

    // The language the Translations filter shows: 0 is every language, then Portuguese, English,
    // Spanish, French, German, Italian, Japanese, Korean and Chinese. The choice is kept per game.
    static int LanguageCount();
    static const char* LanguageName(int index); // English name, for the chip
    int Language() const;
    void SetLanguage(int index);

    // Thumbnails: ask once per entry, then take the bytes when they arrive (false until then).
    void RequestThumb(const std::shared_ptr<ModEntry>& entry);
    bool TakeThumb(const std::shared_ptr<ModEntry>& entry, std::vector<std::uint8_t>& bytes);

    void Install(const std::shared_ptr<ModEntry>& entry);
    bool IsEnabled(const ModEntry& entry) const;
    // Turns an installed mod on or off and saves it. Returns the new state (true = on).
    bool ToggleEnabled(const std::shared_ptr<ModEntry>& entry);

    void RequestDetails(const std::shared_ptr<ModEntry>& entry);
    ModDetails Details(const std::shared_ptr<ModEntry>& entry) const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// Puts the disabled mods of every game into Eden's disabled add-ons list (Settings::values
// .disabled_addons), which PatchManager reads when a game boots. Call it after the settings of
// the session were applied.
void ApplyDisabledMods(const std::filesystem::path& local_state);

} // namespace EdenXbox::Ui
