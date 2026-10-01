// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace EdenXbox::SaveSync {

// Cloud save sync that shares saves with SwitchSaveSync, the Switch homebrew: a real Switch and
// NXbox mirror the same Google Drive folder, so both see one save per game.
//
// Every function here BLOCKS (network and disk) and must be called from a worker thread, never from
// the CoreWindow dispatcher; that thread needs a WinRT apartment (winrt::init_apartment), as the
// game worker thread has. Failures come back in the results, nothing is thrown. Calls that touch a
// save are serialized by an internal lock. There is no UI in this file; the caller shows the
// progress and asks the questions.
//
// Typical use, on one worker thread:
//   const std::string name = GameNameFromNames(nacp.GetApplicationNames());
//   SyncResult r = SyncBeforeBoot(title_id, on_progress, name);
//   if (r.status == SyncStatus::Conflict) {
//       // show r.local_modified / r.cloud_modified, ask the player, then:
//       r = ResolveConflict(title_id, choice, on_progress, name);
//   }
//   ... boot the game, play, close it ...
//   SyncAfterExit(title_id, on_progress, name);
//
// Files, all under LocalState:
//   savesync.json        {"client_id": "...", "client_secret": "..."}, pushed by the owner. NXbox
//                        carries no OAuth credential of its own (SwitchSaveSync keeps its client
//                        out of its public repository), so this file is what makes NXbox use the
//                        SAME OAuth client as the Switch. That matters: the Drive scope is
//                        drive.file, which only shows files created by the same OAuth client.
//                        Optional keys: "endpoint" (an https URL of SwitchSaveSync's auth server,
//                        used instead of client_id/client_secret, the same way SwitchSaveSync's
//                        google.cfg does) and "owner" (the account-folder name to use instead of
//                        the Eden profile name, so it can match the Switch account's nickname).
//   savesync_token.json  the Google refresh token (written by PollDeviceLogin, never logged).
//   savesync_state.json  per save: the fingerprint of the last sync and the cloud folder used.
//   savesync_backup\     the Xbox save as it was before a cloud save replaced it (one copy each).
//   savesync_staging\    scratch space, emptied after each run.

// ---------------------------------------------------------------------------------------------
// Sign-in (OAuth device flow: the TV shows a code, the phone signs in)
// ---------------------------------------------------------------------------------------------

/// True when savesync.json holds a usable credential. When false, sign-in and sync cannot work.
bool IsConfigured();

/// True when a refresh token is stored. It is not validated against Google; a revoked token shows
/// up as SyncStatus::NotSignedIn on the next sync (and the stored token is removed).
bool IsSignedIn();

/// Forgets the stored refresh token (local only, like SwitchSaveSync's sign-out) and cancels any
/// sign-in in progress. Fingerprints in savesync_state.json are kept.
void SignOut();

struct DeviceLogin {
    bool ok = false;
    std::string user_code;        // for example "ABCD-EFGH": show it on the TV
    std::string verification_url; // for example "https://www.google.com/device"
    std::string url_with_code;    // verification_url + "?user_code=" + user_code, for a QR code
    int expires_in_seconds = 0;   // how long the code stays valid
    std::string error;            // English text, only when !ok
};

/// Asks Google for a device code. On success PollDeviceLogin() is the next call.
DeviceLogin StartDeviceLogin();

enum class LoginResult {
    SignedIn,  // the refresh token is stored
    Denied,    // the user refused on the phone
    Expired,   // the code ran out (or Google no longer knows it)
    Cancelled, // CancelDeviceLogin() was called
    Failed,    // no sign-in in progress, bad credential, or Google refused (see eden_uwp_diag.txt)
};

/// Polls Google every few seconds until the user signed in, refused, or the code expired. Blocks
/// for up to expires_in_seconds. Network hiccups are retried, not reported.
LoginResult PollDeviceLogin();

/// Makes a PollDeviceLogin() running on another thread return Cancelled within a fraction of a
/// second. Safe to call at any time.
void CancelDeviceLogin();

// ---------------------------------------------------------------------------------------------
// Saves
// ---------------------------------------------------------------------------------------------

enum class SyncStage {
    Connecting,  // signing in to Google and finding the cloud folder
    Comparing,   // reading the Xbox save and the cloud copy
    Downloading, // cloud -> Xbox, one file per step
    Applying,    // writing the downloaded save into the Eden save folder
    Uploading,   // Xbox -> cloud, one file per step
    Cleaning,    // removing cloud files the Xbox save no longer has
    Finished,
};

struct SyncProgress {
    SyncStage stage = SyncStage::Connecting;
    std::uint32_t done = 0;  // steps finished in this stage
    std::uint32_t total = 0; // steps expected in this stage, 0 when not known yet
    std::string item;        // file being transferred, may be empty
};

/// Called on the calling thread. May be empty.
using ProgressCallback = std::function<void(const SyncProgress&)>;

enum class SyncStatus {
    Failed,        // something went wrong; the Xbox save is unchanged or was put back (see message)
    NotConfigured, // savesync.json is missing or has no usable credential
    NotSignedIn,   // no refresh token, or Google revoked it: sign in again
    NothingToSync, // no save on this console and none in the cloud
    UpToDate,      // both sides already identical; nothing was written
    Uploaded,      // the Xbox save went to the cloud
    Downloaded,    // the cloud save replaced the Xbox save (the old one is in savesync_backup)
    Conflict,      // both sides changed since the last sync: NOTHING was written, the UI must ask
};

struct SyncResult {
    SyncStatus status = SyncStatus::Failed;
    std::string message;      // short English text for logs and as a fallback
    std::string cloud_folder; // "<game folder>/<account folder>" when known

    // Filled whenever the side was read; the UI shows them next to a Conflict. Times are UTC
    // seconds since 1970-01-01, 0 when unknown: the newest file time on the Xbox, and the newest
    // Drive modifiedTime in the cloud folder.
    std::int64_t local_modified = 0;
    std::int64_t cloud_modified = 0;
    std::uint32_t local_files = 0;
    std::uint32_t cloud_files = 0;
    std::uint64_t local_bytes = 0;
    std::uint64_t cloud_bytes = 0;
};

/// Mirrors one title's save for the ACTIVE Eden user:
/// nand\user\save\0000000000000000\<user>\<TITLEID>.
/// Call it before the game boots; it downloads a newer cloud save (and also uploads when only the
/// Xbox changed). A Conflict result means nothing was written: ask, then call ResolveConflict.
///
/// The active user is Settings::values.current_user (through Eden's ProfileManager), so call this
/// after the settings of the session (eden_settings.txt) were applied.
///
/// title_id is the id Eden names the save folder with (the application's program id).
/// game_name is the game's name exactly as SwitchSaveSync names it: GameNameFromNames() over the
/// NACP names. It may be empty only after one successful sync of this save, because the cloud
/// folder is then remembered in savesync_state.json.
SyncResult SyncBeforeBoot(std::uint64_t title_id, const ProgressCallback& progress = {},
                          const std::string& game_name = {});

/// The same reconciliation, meant for after the game closed and Eden released the save files.
SyncResult SyncAfterExit(std::uint64_t title_id, const ProgressCallback& progress = {},
                         const std::string& game_name = {});

enum class ConflictChoice {
    KeepXbox,  // the Xbox save overwrites the cloud copy
    KeepCloud, // the cloud copy overwrites the Xbox save (the old one goes to savesync_backup)
};

/// Applies the user's answer to a Conflict. It refuses (Failed) to replace a side with nothing.
SyncResult ResolveConflict(std::uint64_t title_id, ConflictChoice choice,
                           const ProgressCallback& progress = {},
                           const std::string& game_name = {});

/// The game name SwitchSaveSync uses for a title: the first non-empty application name among the
/// first 16 NACP language entries (FileSys::NACP::GetApplicationNames()), independent of the
/// console language. Empty when there is none.
std::string GameNameFromNames(const std::vector<std::string>& nacp_application_names);

} // namespace EdenXbox::SaveSync
