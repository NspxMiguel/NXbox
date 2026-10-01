// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {

// The screens around SwitchSaveSync (docs/nxbox-ui.md, Increment 4). The backend is
// eden_uwp/save_sync.h; everything here is the part the player sees. The functions that take a
// Renderer draw on the library's window; the ones that take only the window set up their own
// renderer and release it before they return, so Mesa can take the window afterwards.

// The game a sync is about.
struct SyncGame {
    std::string title_id;     // 16 hex digits, the id Eden names the save folder with
    std::string name;         // GameNameFromNames over the NACP names, UTF-8
    std::wstring display_name; // what the screen shows
};

enum class SyncAccount {
    NotConfigured, // LocalState\savesync.json is missing: sign-in cannot work
    SignedOut,
    SignedIn,
};

SyncAccount GetSyncAccount();
// True when saves sync: configured and signed in.
bool SyncEnabled();

// LocalState\setup_done.txt: written when the first-run setup has been shown and answered.
bool IsSetupDone(const std::filesystem::path& local_state);

// The first-run question "Sincronizar saves com o SwitchSaveSync?" with Sim and Agora não; Sim
// goes on to the sign-in screen. Writes setup_done.txt at the end. Returns true when the window was
// closed meanwhile (setup_done.txt is then not written, so the question comes back).
bool RunFirstRunSetup(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                      Input& input, const std::filesystem::path& local_state);

enum class SignInOutcome { SignedIn, Denied, Expired, Cancelled, Failed, NotConfigured };

// The device-flow sign-in: the user code large, the address to open on the phone, and the result.
// `window_closed` is set when the window closed meanwhile. Without a savesync.json it explains the
// missing file and returns NotConfigured.
SignInOutcome RunSignIn(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                        Input& input, bool& window_closed);

// Before a chosen game boots, with sync enabled: downloads a newer cloud save (and uploads when
// only the Xbox changed) behind a progress screen, asks which save to keep on a conflict, and
// notes that an upload is owed until SyncAfterExitHeadless has done it. Does nothing when sync is
// off. `settings` of the session (eden_settings.txt) must be applied already, because the active
// profile decides which save folder is synced. Returns true when the window was closed.
bool RunBootSync(const winrt::Windows::UI::Core::CoreWindow& window,
                 const std::filesystem::path& local_state, const SyncGame& game);

// An upload still owed from the last session (the app was closed or suspended before the game's
// save went to the cloud): done now, with a progress screen, before the library shows. Returns
// true when the window was closed.
bool RunPendingSync(const winrt::Windows::UI::Core::CoreWindow& window,
                    const std::filesystem::path& local_state);
// True when LocalState\savesync_pending.txt names a game.
bool HasPendingSync(const std::filesystem::path& local_state);

// After the game closed and Eden released the save files: uploads the Xbox save, with no screen
// (nothing owns the window at that point). Blocks; call it on the game's worker thread, which has a
// WinRT apartment. The debt noted by RunBootSync is cleared when it succeeds.
void SyncAfterExitHeadless(const std::filesystem::path& local_state, const SyncGame& game);

} // namespace EdenXbox::Ui
