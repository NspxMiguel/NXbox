// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden file system headers #undef the A/W macros of the Win32
// calls they name their methods after.
#include <windows.h>

#include <fileapifromapp.h>

#include "eden_uwp/ui/usb_import_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.h>

#include "common/fs/file.h"
#include "common/scope_exit.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;
namespace FS = Common::FS;

using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::Storage::KnownFolders;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::StorageFolder;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// Layout, in canvas units. The rows follow the mod store's list.
constexpr float kMargin = kScreenMargin;
constexpr float kRight = kCanvasWidth - kScreenMargin;
constexpr float kTitleTop = 150.0f;
constexpr float kTitleHeight = 76.0f;
constexpr float kSpaceTop = 232.0f;
constexpr float kSpaceHeight = 30.0f;
constexpr float kAllTop = 290.0f;
constexpr float kListTop = 390.0f;
constexpr float kRowHeight = 104.0f;
constexpr float kRowPitch = 110.0f;
constexpr float kRowRadius = 22.0f;
constexpr float kRowPadding = 28.0f;
constexpr float kStatRight = kRight - kRowPadding;
constexpr float kStatWidth = 220.0f;
constexpr int kVisibleRows = 5;

constexpr float kSheetLeft = 420.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kChoiceTop = 300.0f;
constexpr float kChoiceHeight = 460.0f;
constexpr float kWorkTop = 340.0f;
constexpr float kWorkHeight = 360.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(4200);

constexpr int kMaxDepth = 3;                          // folder levels below the drive root
constexpr std::size_t kChunk = 4u << 20;              // 4 MiB per read/write
constexpr std::uint64_t kSpaceReserve = 64u << 20;    // left free on the console after a copy
constexpr auto kRateWindow = std::chrono::milliseconds(500);

struct Item {
    bool is_key = false;
    bool already_copied = false;
    std::wstring path;   // full path on the drive
    std::wstring name;   // file name, kept as the name of the copy
    std::wstring drive;  // the drive's root, e.g. L"E:\"
    std::wstring letter; // L"E:"
    std::uint64_t size = 0;
};

struct Drive {
    std::wstring root;
    std::wstring name;
    bool unseen = false;
    std::wstring letter;
    std::optional<std::uint64_t> free;
};

struct ScanResult {
    std::vector<Drive> drives;
    std::vector<Item> items;
    std::optional<std::uint64_t> xbox_free;
};

struct Job {
    Item item;
    bool move = false; // false: copy to the console's storage
};

enum class Phase { Scanning, Ready, Working, Detected, Result };
enum class Outcome { Done, Failed, Cancelled };

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return text;
}

std::wstring Extension(const std::wstring& name) {
    const std::size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : Lower(name.substr(dot));
}

bool IsGameName(const std::wstring& name) {
    const std::wstring extension = Extension(name);
    return extension == L".nsp" || extension == L".nsz" || extension == L".xci" ||
           extension == L".xcz";
}

bool IsKeyName(const std::wstring& name) {
    const std::wstring lower = Lower(name);
    return lower == L"prod.keys" || lower == L"title.keys";
}

bool IsHiddenOrSystem(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    return (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

bool FileExists(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &data) != 0;
}

std::string Utf8(const std::wstring& text) {
    return winrt::to_string(text);
}

std::wstring TrimBackslash(std::wstring path) {
    while (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

// Collects the games and keys of one folder and of the folders below it, down to kMaxDepth.
// `skip` is the lower-case path of the library folder on this drive, which needs no import.
void Walk(const StorageFolder& folder, int depth, const Drive& drive, const std::wstring& skip,
          const std::atomic<bool>& cancel, std::vector<Item>& items) {
    if (cancel.load()) {
        return;
    }
    try {
        const auto files = AwaitBounded(folder.GetFilesAsync(), std::chrono::seconds(10));
        if (files) {
            for (const auto& file : *files) {
                if (cancel.load()) {
                    return;
                }
                const std::wstring name = std::wstring(file.Name());
                const bool key = IsKeyName(name);
                if (!key && !IsGameName(name)) {
                    continue;
                }
                const auto properties =
                    AwaitBounded(file.GetBasicPropertiesAsync(), std::chrono::seconds(5));
                if (!properties) {
                    continue;
                }
                Item item;
                item.is_key = key;
                item.path = std::wstring(file.Path());
                item.name = name;
                item.drive = drive.root;
                item.letter = drive.letter;
                item.size = (*properties).Size();
                items.push_back(std::move(item));
            }
        }
        if (depth >= kMaxDepth) {
            return;
        }
        const auto folders = AwaitBounded(folder.GetFoldersAsync(), std::chrono::seconds(10));
        if (!folders) {
            return;
        }
        for (const auto& sub : *folders) {
            const std::wstring name = std::wstring(sub.Name());
            const std::wstring path = std::wstring(sub.Path());
            if (name.empty() || name.front() == L'.' || name.front() == L'$' ||
                Lower(name) == L"system volume information" || Lower(path) == skip ||
                IsHiddenOrSystem(path)) {
                continue;
            }
            Walk(sub, depth + 1, drive, skip, cancel, items);
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_IMPORT_SCAN skip " + winrt::to_string(error.message()));
    }
}

ScanResult ScanDrives(const fs::path& local_state, const std::atomic<bool>& cancel,
                      std::set<std::wstring>* connected = nullptr) {
    ScanResult result;
    if (!connected)
        result.xbox_free = FreeSpace(local_state);
    try {
        const auto drives =
            AwaitBounded(KnownFolders::RemovableDevices().GetFoldersAsync(), std::chrono::seconds(5));
        if (drives) {
            std::set<std::wstring> present;
            for (const auto& folder : *drives) {
                if (cancel.load()) {
                    break;
                }
                Drive drive;
                drive.root = std::wstring(folder.Path());
                drive.name = std::wstring(folder.DisplayName());
                if (drive.root.empty()) {
                    Diagnostic("USB_DETECT skipping drive without a folder path");
                    continue;
                }
                const auto identity = drive.name + L"|" + Lower(drive.root);
                present.insert(identity);
                if (connected && connected->contains(identity)) {
                    continue;
                }
                drive.letter = drive.root.size() >= 2 && drive.root[1] == L':'
                                   ? drive.root.substr(0, 2)
                                   : std::wstring(folder.DisplayName());
                drive.free = StorageFreeSpace(fs::path(drive.root));
                const std::wstring skip = Lower(TrimBackslash(drive.root)) + L"\\nxbox\\games";
                Walk(folder, 0, drive, connected ? L"" : skip, cancel, result.items);
                result.drives.push_back(std::move(drive));
            }
            if (connected) {
                *connected = std::move(present);
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("USB_IMPORT_SCAN failed " + winrt::to_string(error.message()));
    }
    if (connected) {
        for (auto& item : result.items) {
            const auto target = local_state / (item.is_key ? L"eden/keys" : L"games") / item.name;
            std::error_code error;
            const auto size = fs::file_size(target, error);
            item.already_copied = !error && size == item.size;
        }
    }
    // Keys first, then the games by name.
    std::sort(result.items.begin(), result.items.end(), [](const Item& a, const Item& b) {
        if (a.is_key != b.is_key) {
            return a.is_key;
        }
        return Lower(a.name) < Lower(b.name);
    });
    Diagnostic("USB_IMPORT_SCAN drives=" + std::to_string(result.drives.size()) +
               " files=" + std::to_string(result.items.size()));
    return result;
}

const char* ModeName(UsbMode mode) {
    switch (mode) {
    case UsbMode::Ask:
        return "ask";
    case UsbMode::Copy:
        return "copy";
    case UsbMode::External:
        return "external";
    case UsbMode::Off:
        return "off";
    default:
        return "unset";
    }
}

bool InDriveLibrary(const Item& item) {
    const auto prefix = Lower(TrimBackslash(item.drive)) + L"\\nxbox\\games\\";
    return Lower(item.path).starts_with(prefix);
}

std::string DriveIdentity(const Drive& drive) {
    return Utf8(drive.name + L"|" + Lower(drive.root));
}

void RememberDrive(const fs::path& local_state, const Drive& drive) {
    std::ofstream file(local_state / "usb_seen.txt", std::ios::app);
    file << std::quoted(DriveIdentity(drive)) << '\n';
    if (!file)
        Diagnostic("USB_DETECT failed saving seen drive");
}

class UsbImportScreen {
public:
    UsbImportScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                    const fs::path& local_state)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state) {}

    UsbImportScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                    const fs::path& local_state, Drive drive, std::vector<Item> items, UsbMode mode)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state),
          detected_(true), mode_(mode) {
        drives_.push_back(std::move(drive));
        items_ = std::move(items);
    }

    ~UsbImportScreen() {
        cancel_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Returns true when the window was closed.
    bool Run() {
        const auto closed_token =
            window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(closed_token);
        };
        Diagnostic("UI usb import screen open");
        if (detected_) {
            const Drive& drive = drives_.front();
            phase_ = Phase::Detected;
            if (mode_ != UsbMode::Unset && mode_ != UsbMode::Ask && !drive.unseen) {
                StartDetectedAction(mode_);
            } else {
                if (drive.unseen)
                    RememberDrive(local_state_, drive);
            }
        } else {
            StartScan();
        }
        while (!closed_ && !leaving_) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        Diagnostic("UI usb import screen closed");
        return closed_;
    }

private:
    // ---- Worker threads ----
    //
    // One worker at a time: a scan, or a batch of copies and moves. Everything that touches a
    // StorageFolder or the removable drives runs there, in its own MTA apartment.

    template <typename Body>
    void StartWorker(Body body) {
        if (worker_.joinable()) {
            worker_.join();
        }
        done_.store(false);
        worker_ = std::thread([this, body = std::move(body)]() mutable {
            struct Finish {
                std::atomic<bool>& flag;
                ~Finish() {
                    flag.store(true);
                }
            } finish{done_};
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment {
                    ~Apartment() {
                        winrt::uninit_apartment();
                    }
                } apartment;
                body();
            } catch (const winrt::hresult_error& error) {
                Diagnostic("USB_IMPORT worker failed " + winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                Diagnostic(std::string("USB_IMPORT worker failed ") + error.what());
            }
        });
    }

    void StartScan() {
        phase_ = Phase::Scanning;
        items_.clear();
        drives_.clear();
        selected_ = 0;
        top_ = 0;
        focus_all_ = false;
        choice_open_ = false;
        scroll_ = Tween(0.0f);
        cancel_.store(false); // a cancelled copy must not stop the scan that follows it
        StartWorker([this] {
            ScanResult result = ScanDrives(local_state_, cancel_);
            const std::lock_guard lock(mutex_);
            scan_ = std::move(result);
        });
    }

    void StartJobs(std::vector<Job> jobs, int skipped) {
        if (jobs.empty() && detected_) {
            Diagnostic(std::string("USB_AUTO ") + ModeName(action_) + " done=0 skipped=0");
            leaving_ = true;
            return;
        }
        if (jobs.empty() && !detected_) {
            return;
        }
        phase_ = Phase::Working;
        choice_open_ = false;
        cancel_.store(false);
        ok_.store(0);
        failed_.store(0);
        skipped_.store(skipped);
        cancelled_.store(false);
        job_total_.store(static_cast<int>(jobs.size()));
        job_index_.store(0);
        bytes_done_.store(0);
        bytes_size_.store(0);
        rate_.store(0.0);
        single_move_ = jobs.size() == 1 && jobs.front().move;
        StartWorker([this, jobs = std::move(jobs)] {
            for (std::size_t i = 0; i < jobs.size(); ++i) {
                if (cancel_.load()) {
                    cancelled_.store(true);
                    break;
                }
                const auto& job = jobs[i];
                if (!job.move) {
                    const auto target = local_state_ / (job.item.is_key ? L"eden/keys" : L"games");
                    std::error_code error;
                    const auto size = fs::file_size(target / job.item.name, error);
                    if (!error && size == job.item.size)
                        continue;
                    const auto free = FreeSpace(local_state_);
                    if (!job.item.is_key &&
                        (!free || *free < kSpaceReserve || job.item.size > *free - kSpaceReserve)) {
                        skipped_.fetch_add(1);
                        continue;
                    }
                }
                job_index_.store(static_cast<int>(i) + 1);
                Outcome outcome = RunJob(jobs[i]);
                if (outcome == Outcome::Done) {
                    ok_.fetch_add(1);
                } else if (outcome == Outcome::Failed) {
                    failed_.fetch_add(1);
                } else {
                    cancelled_.store(true);
                    break;
                }
            }
        });
    }

    Outcome RunJob(const Job& job) {
        {
            const std::lock_guard lock(mutex_);
            job_name_ = job.item.name;
        }
        moving_.store(job.move);
        bytes_done_.store(0);
        bytes_size_.store(job.move ? 0 : job.item.size);
        rate_.store(0.0);
        std::string reason;
        Outcome outcome = Outcome::Failed;
        if (job.move) {
            outcome = MoveToDrive(job.item, reason) ? Outcome::Done : Outcome::Failed;
            Diagnostic("USB_IMPORT_MOVE " + Utf8(job.item.name) +
                       (outcome == Outcome::Done ? " ok" : " failed " + reason));
        } else {
            const fs::path target =
                job.item.is_key ? local_state_ / L"eden" / L"keys" : local_state_ / L"games";
            outcome = CopyToConsole(job.item, target, reason);
            Diagnostic("USB_IMPORT_COPY " + Utf8(job.item.name) +
                       (outcome == Outcome::Done        ? " ok"
                        : outcome == Outcome::Cancelled ? " failed cancelled"
                                                        : " failed " + reason));
        }
        return outcome;
    }

    // Copies in 4 MiB chunks into "<name>.partial", checks the size, then renames it into place.
    // A partial file never stays behind. The source is read through IOFile, which falls back to
    // CreateFileFromAppW for the drive.
    Outcome CopyToConsole(const Item& item, const fs::path& target_dir, std::string& reason) {
        std::error_code error;
        fs::create_directories(target_dir, error);
        const fs::path final_path = target_dir / item.name;
        const fs::path partial = target_dir / (item.name + L".partial");
        const auto existing = fs::file_size(final_path, error);
        if (!error && existing == item.size) {
            return Outcome::Done; // the same file is already there
        }
        const auto discard = [&partial] {
            std::error_code ignored;
            fs::remove(partial, ignored);
        };
        {
            FS::IOFile in(fs::path(item.path), FS::FileAccessMode::Read);
            if (!in.IsOpen()) {
                reason = "cannot open the source";
                return Outcome::Failed;
            }
            FS::IOFile out(partial, FS::FileAccessMode::Write);
            if (!out.IsOpen()) {
                reason = "cannot create the copy";
                return Outcome::Failed;
            }
            std::vector<std::uint8_t> buffer(kChunk);
            std::uint64_t copied = 0;
            std::uint64_t sample_bytes = 0;
            Clock::time_point sample_at = Clock::now();
            while (copied < item.size) {
                if (cancel_.load()) {
                    out.Close();
                    discard();
                    return Outcome::Cancelled;
                }
                const std::size_t want =
                    static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, item.size - copied));
                const std::size_t got = in.ReadSpan(std::span<std::uint8_t>(buffer.data(), want));
                if (got == 0) {
                    out.Close();
                    discard();
                    reason = "read failed";
                    return Outcome::Failed;
                }
                if (out.WriteSpan(std::span<const std::uint8_t>(buffer.data(), got)) != got) {
                    out.Close();
                    discard();
                    reason = "write failed (disk full?)";
                    return Outcome::Failed;
                }
                copied += got;
                bytes_done_.store(copied);
                const Clock::time_point now = Clock::now();
                if (now - sample_at >= kRateWindow) {
                    rate_.store(static_cast<double>(copied - sample_bytes) /
                                std::chrono::duration<double>(now - sample_at).count());
                    sample_at = now;
                    sample_bytes = copied;
                }
            }
            if (!out.Commit()) {
                out.Close();
                discard();
                reason = "flush failed (disk full?)";
                return Outcome::Failed;
            }
        }
        // Verified by size before it counts.
        const auto written = fs::file_size(partial, error);
        if (error || written != item.size) {
            discard();
            reason = "size mismatch";
            return Outcome::Failed;
        }
        fs::remove(final_path, error); // replaces an older, different file
        fs::rename(partial, final_path, error);
        if (error) {
            discard();
            reason = "cannot rename: " + error.message();
            return Outcome::Failed;
        }
        return Outcome::Done;
    }

    // A move inside the same drive, into <drive>\NXbox\games, so the library lists it without
    // using the console's storage.
    bool MoveToDrive(const Item& item, std::string& reason) {
        try {
            const auto drive = AwaitBounded(StorageFolder::GetFolderFromPathAsync(item.drive),
                                            std::chrono::seconds(5));
            if (!drive) {
                reason = "drive not reachable";
                return false;
            }
            const auto root = AwaitBounded(
                (*drive).CreateFolderAsync(L"NXbox",
                                           winrt::Windows::Storage::CreationCollisionOption::OpenIfExists),
                std::chrono::seconds(5));
            if (!root) {
                reason = "cannot create NXbox";
                return false;
            }
            const auto games = AwaitBounded(
                (*root).CreateFolderAsync(L"games",
                                          winrt::Windows::Storage::CreationCollisionOption::OpenIfExists),
                std::chrono::seconds(5));
            if (!games) {
                reason = "cannot create NXbox\\games";
                return false;
            }
            const std::wstring target =
                TrimBackslash(std::wstring((*games).Path())) + L"\\" + item.name;
            if (FileExists(target)) {
                reason = "target exists";
                return false;
            }
            if (MoveFileFromAppW(item.path.c_str(), target.c_str())) {
                return true;
            }
            const DWORD win32_error = GetLastError();
            // The broker can refuse the Win32 call where the storage API still works.
            const auto file =
                AwaitBounded(StorageFile::GetFileFromPathAsync(item.path), std::chrono::seconds(5));
            if (file) {
                auto move = (*file).MoveAsync(
                    *games, item.name, winrt::Windows::Storage::NameCollisionOption::FailIfExists);
                if (move.wait_for(std::chrono::seconds(60)) ==
                    winrt::Windows::Foundation::AsyncStatus::Completed) {
                    return true;
                }
                move.Cancel();
            }
            reason = "move refused, error=" + std::to_string(win32_error);
        } catch (const winrt::hresult_error& error) {
            reason = winrt::to_string(error.message());
        }
        return false;
    }

    // ---- State and input ----

    int RowCount() const {
        return static_cast<int>(items_.size());
    }

    std::size_t Index(int row) const {
        return static_cast<std::size_t>(row);
    }

    bool HasGames() const {
        return std::any_of(items_.begin(), items_.end(),
                           [](const Item& item) { return !item.is_key; });
    }

    // Picks up what the worker finished.
    void AdoptWorker(Clock::time_point now) {
        if (!worker_.joinable() || !done_.load()) {
            return;
        }
        worker_.join();
        if (phase_ == Phase::Scanning) {
            const std::lock_guard lock(mutex_);
            if (scan_) {
                items_ = std::move(scan_->items);
                drives_ = std::move(scan_->drives);
                xbox_free_ = scan_->xbox_free;
                scan_.reset();
            }
            phase_ = Phase::Ready;
            selected_ = std::clamp(selected_, 0, std::max(RowCount() - 1, 0));
        } else if (phase_ == Phase::Working) {
            if (detected_) {
                Diagnostic(std::string("USB_AUTO ") + ModeName(action_) +
                           " done=" + std::to_string(ok_.load()) +
                           " skipped=" + std::to_string(skipped_.load()));
                result_ = Summary();
                phase_ = Phase::Result;
                if (result_.empty())
                    leaving_ = true;
                return;
            }
            Toast(Summary(), now);
            // The files moved or arrived: look again, which also refreshes the free space.
            StartScan();
        }
    }

    std::wstring Summary() const {
        if (cancelled_.load()) {
            return Tr(Text::UsbCancelled);
        }
        const int ok = ok_.load();
        const int failed = failed_.load();
        const int skipped = skipped_.load();
        if (single_move_) {
            return ok > 0 ? Tr(Text::UsbMoved) : Tr(Text::UsbMoveFailed);
        }
        std::wstring text;
        const auto add = [&text](int count, const wchar_t* label) {
            if (count > 0) {
                text += (text.empty() ? L"" : L" · ") + std::to_wstring(count) + L" " + label;
            }
        };
        add(ok, Tr(Text::UsbSummaryOk));
        add(failed, Tr(Text::UsbSummaryFailed));
        add(skipped, Tr(detected_ ? Text::UsbDidNotFit : Text::UsbSummaryNoSpace));
        return text;
    }

    void Update(Clock::time_point now) {
        AdoptWorker(now);
        switch (phase_) {
        case Phase::Detected:
            if (input_.Pressed(Button::Up))
                choice_focus_ = std::max(0, choice_focus_ - 1);
            if (input_.Pressed(Button::Down))
                choice_focus_ = std::min(2, choice_focus_ + 1);
            if (input_.Pressed(Button::B) || (input_.Pressed(Button::A) && choice_focus_ == 2)) {
                Diagnostic("USB_DETECT_CHOICE later");
                leaving_ = true;
            } else if (input_.Pressed(Button::A)) {
                const auto action = choice_focus_ == 0 ? UsbMode::Copy : UsbMode::External;
                Diagnostic(std::string("USB_DETECT_CHOICE ") + ModeName(action));
                if (mode_ != UsbMode::Ask && !SaveUsbMode(local_state_, action)) {
                    choice_message_ = Tr(Text::UsbSaveFailed);
                    return;
                }
                StartDetectedAction(action);
            }
            break;
        case Phase::Result:
            if (input_.Pressed(Button::A) || input_.Pressed(Button::B))
                leaving_ = true;
            break;
        case Phase::Scanning:
            if (input_.Pressed(Button::B)) {
                cancel_.store(true);
                leaving_ = true;
            }
            break;
        case Phase::Working:
            if (input_.Pressed(Button::B)) {
                cancel_.store(true); // the worker deletes the partial file and stops
            }
            break;
        case Phase::Ready:
            if (choice_open_) {
                HandleChoiceInput(now);
            } else {
                HandleInput(now);
            }
            break;
        }
        selected_ = std::clamp(selected_, 0, std::max(RowCount() - 1, 0));
        if (selected_ < top_) {
            top_ = selected_;
        } else if (selected_ > top_ + kVisibleRows - 1) {
            top_ = selected_ - (kVisibleRows - 1);
        }
        top_ = std::max(top_, 0);
        if (scroll_.Target() != static_cast<float>(top_)) {
            scroll_.To(static_cast<float>(top_), now, kDurationPanel);
        }
    }

    void StartDetectedAction(UsbMode action) {
        action_ = action;
        std::vector<Job> jobs;
        for (const auto& item : items_) {
            if (action == UsbMode::External && !item.is_key && InDriveLibrary(item))
                continue;
            const bool move = action == UsbMode::External && !item.is_key;
            if (!move && item.already_copied)
                continue;
            jobs.push_back({item, move});
        }
        StartJobs(std::move(jobs), 0);
    }

    void DrawDetection() {
        const auto sheet = RectF(300.0f, 150.0f, 1620.0f, 930.0f);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Tr(Text::UsbDetected), Font::Heading,
                             RectF(left, 185.0f, right, 255.0f), Theme::kText);
        const auto& drive = drives_.front();
        const auto games = std::count_if(items_.begin(), items_.end(), [](const Item& item) {
            return !item.is_key && !InDriveLibrary(item);
        });
        const auto keys = std::count_if(items_.begin(), items_.end(), [](const Item& item) {
            return item.is_key && !InDriveLibrary(item);
        });
        const auto detail = drive.name + L" · " +
                            (drive.free ? FormatBytes(*drive.free) + L" " + Tr(Text::UsbFree)
                                        : std::wstring(Tr(Text::UsbSpaceUnknown))) +
                            L"\n" + std::to_wstring(games) + L" " + Tr(Text::UsbDetectedGames) +
                            L" · " + std::to_wstring(keys) + L" " + Tr(Text::UsbDetectedKeys);
        renderer_.DrawString(detail, Font::Body, RectF(left, 270.0f, right, 395.0f),
                             Theme::kTextSecondary);
        const Text labels[] = {Text::UsbModeCopy, Text::UsbModeExternal, Text::UsbLater};
        for (int i = 0; i < 3; ++i) {
            const float top = 420.0f + static_cast<float>(i) * 96.0f;
            DrawChoicePill(renderer_, RectF(left, top, right, top + kPillHeight), Tr(labels[i]),
                           i == 0, choice_focus_ == i);
        }
        renderer_.DrawString(choice_message_.empty()
                                 ? Tr(mode_ == UsbMode::Ask ? Text::UsbRepeatAsk : Text::UsbRepeat)
                                 : choice_message_.c_str(),
                             Font::Body, RectF(left, 740.0f, right, 900.0f), Theme::kTextSecondary);
    }

    void HandleInput(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            leaving_ = true;
            return;
        }
        if (input_.Pressed(Button::X)) {
            StartScan();
            return;
        }
        if (RowCount() == 0) {
            if (input_.Pressed(Button::A)) {
                StartScan();
            }
            return;
        }
        if (input_.Pressed(Button::Up)) {
            if (focus_all_) {
                return;
            }
            if (selected_ == 0 && HasGames()) {
                focus_all_ = true;
            } else {
                selected_ = std::max(selected_ - 1, 0);
            }
        }
        if (input_.Pressed(Button::Down)) {
            if (focus_all_) {
                focus_all_ = false;
            } else {
                selected_ = std::min(selected_ + 1, RowCount() - 1);
            }
        }
        if (input_.Pressed(Button::A)) {
            if (focus_all_) {
                CopyAll(now);
            } else {
                Activate(now);
            }
        }
    }

    void HandleChoiceInput(Clock::time_point now) {
        if (input_.Pressed(Button::B)) {
            choice_open_ = false;
            return;
        }
        if (input_.Pressed(Button::Left) || input_.Pressed(Button::Right)) {
            choice_focus_ = 1 - choice_focus_;
        }
        if (!input_.Pressed(Button::A)) {
            return;
        }
        const Item& item = items_[Index(selected_)];
        if (choice_focus_ == 1) {
            StartJobs({Job{item, true}}, 0);
            return;
        }
        if (xbox_free_ && item.size + kSpaceReserve > *xbox_free_) {
            // Refused up front: say so and put the focus on the other answer.
            choice_message_ = std::wstring(Tr(Text::UsbNoSpace)) + L" " + Tr(Text::UsbNeeds) +
                              L" " + FormatBytes(item.size) + L", " + FormatBytes(*xbox_free_) +
                              L" " + Tr(Text::UsbFree) + L".";
            choice_focus_ = 1;
            Diagnostic("USB_IMPORT_COPY " + Utf8(item.name) + " failed not enough space");
            return;
        }
        StartJobs({Job{item, false}}, 0);
        (void)now;
    }

    void Activate(Clock::time_point now) {
        const Item& item = items_[Index(selected_)];
        if (item.is_key) {
            StartJobs({Job{item, false}}, 0); // keys are small: copied straight away
            return;
        }
        choice_open_ = true;
        choice_focus_ = 0;
        choice_message_.clear();
        (void)now;
    }

    // Every key and every game that still fits on the console's storage.
    void CopyAll(Clock::time_point now) {
        std::vector<Job> jobs;
        for (const Item& item : items_) {
            jobs.push_back(Job{item, false});
        }
        // Check duplicates and fresh free space on the worker before each copy.
        StartJobs(std::move(jobs), 0);
        (void)now;
    }

    void Toast(const std::wstring& text, Clock::time_point now) {
        if (text.empty()) {
            return;
        }
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        if (detected_ && phase_ == Phase::Detected) {
            DrawDetection();
            DrawHints(renderer_, {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                                  {Theme::kButtonB, L"B", Tr(Text::UsbLater)}});
            return;
        }
        if (detected_ && phase_ == Phase::Result) {
            DrawHeader();
            DrawMessage(result_);
            DrawHints(renderer_, {{Theme::kButtonB, L"B", Tr(Text::HintBack)}});
            return;
        }
        DrawHeader();
        if (phase_ == Phase::Scanning) {
            DrawMessage(Tr(Text::UsbScanning));
        } else if (RowCount() == 0 && phase_ == Phase::Ready) {
            DrawMessage(drives_.empty() ? Tr(Text::UsbNoDrive) : Tr(Text::UsbEmpty));
        } else {
            DrawCopyAll();
            DrawList(now);
        }
        DrawTabs(renderer_, 0, false);
        DrawClock(renderer_);
        DrawToast(now);
        if (phase_ == Phase::Ready && choice_open_ && RowCount() > 0) {
            DrawChoice();
        }
        if (phase_ == Phase::Working) {
            DrawProgress();
        }
        DrawHints(renderer_, CurrentHints());
    }

    void DrawHeader() {
        renderer_.DrawString(Tr(Text::UsbTitle), Font::ModsTitle,
                             RectF(kMargin, kTitleTop, kMargin + 1200.0f, kTitleTop + kTitleHeight),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        // Free space of the console and of each drive: "Xbox 4,8 GB free · E: 120,0 GB free".
        const auto segment = [](const std::wstring& label, std::optional<std::uint64_t> free) {
            return label + L" " + (free ? FormatBytes(*free) : std::wstring(L"?")) + L" " +
                   Tr(Text::UsbFree);
        };
        std::wstring line = segment(Tr(Text::UsbXbox), xbox_free_);
        for (const Drive& drive : drives_) {
            line += L"  ·  " + segment(drive.letter, drive.free);
        }
        renderer_.DrawString(line, Font::Meta,
                             RectF(kMargin, kSpaceTop, kRight, kSpaceTop + kSpaceHeight),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
    }

    void DrawMessage(const std::wstring& text) {
        renderer_.DrawString(text, Font::Body,
                             RectF(kMargin, kAllTop + 20.0f, kMargin + 1200.0f, kAllTop + 240.0f),
                             Theme::kTextSecondary);
    }

    void DrawCopyAll() {
        if (!HasGames()) {
            return;
        }
        const std::wstring label = Tr(Text::UsbCopyAll);
        const float width = PillWidth(renderer_, label, false);
        DrawChoicePill(renderer_, RectF(kMargin, kAllTop, kMargin + width, kAllTop + kPillHeight),
                       label, false, phase_ == Phase::Ready && !choice_open_ && focus_all_);
    }

    void DrawList(Clock::time_point now) {
        const float scroll = scroll_.Value(now);
        renderer_.PushClip(RectF(kMargin - 20.0f, kListTop - 10.0f, kRight + 20.0f,
                                 kListTop + static_cast<float>(kVisibleRows) * kRowPitch - 1.0f));
        for (int i = 0; i < RowCount(); ++i) {
            const float y = kListTop + (static_cast<float>(i) - scroll) * kRowPitch;
            if (y + kRowHeight < kListTop - 12.0f ||
                y > kListTop + static_cast<float>(kVisibleRows) * kRowPitch) {
                continue;
            }
            DrawRow(items_[Index(i)], i, y,
                    i == selected_ && !focus_all_ && phase_ == Phase::Ready && !choice_open_);
        }
        renderer_.PopClip();
    }

    void DrawRow(const Item& item, int index, float y, bool focused) {
        const D2D1_RECT_F row = RectF(kMargin, y, kRight, y + kRowHeight);
        if (focused) {
            renderer_.FillRounded(row, kRowRadius, Theme::kSurfaceStrong);
        } else if (index > 0 && index - 1 != selected_) {
            renderer_.FillRounded(RectF(kMargin, y - 3.0f, kRight, y - 2.0f), 0.0f,
                                  Theme::kHairline);
        }
        std::wstring kind = item.is_key ? std::wstring(Tr(Text::UsbKeysTag))
                                        : Extension(item.name);
        if (!item.is_key && !kind.empty()) {
            kind.erase(0, 1);
            std::transform(kind.begin(), kind.end(), kind.begin(), [](wchar_t letter) {
                return static_cast<wchar_t>(std::towupper(letter));
            });
        }
        const float left = kMargin + kRowPadding;
        const float info_right = kStatRight - kStatWidth - 24.0f;
        renderer_.DrawString(item.name, Font::RowTitle, RectF(left, y + 20.0f, info_right, y + 54.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(item.letter + L" · " + kind, Font::RowSub,
                             RectF(left, y + 58.0f, info_right, y + 84.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(FormatBytes(item.size), Font::Stat,
                             RectF(kStatRight - kStatWidth, y, kStatRight, y + kRowHeight),
                             Theme::kText, HAlign::Right, VAlign::Middle);
        if (focused) {
            DrawRing(renderer_, row, kRowRadius + kRingGap, 1.0f);
        }
    }

    // The choice for a game: copy it to the console or play it from the drive.
    void DrawChoice() {
        const Item& item = items_[Index(selected_)];
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kChoiceTop, kSheetLeft + kSheetWidth, kChoiceTop + kChoiceHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(item.name, Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + 100.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(Tr(Text::UsbChoiceBody), Font::Body,
                             RectF(left, sheet.top + 120.0f, right, sheet.top + 250.0f),
                             Theme::kTextSecondary);
        if (!choice_message_.empty()) {
            renderer_.DrawString(choice_message_, Font::Meta,
                                 RectF(left, sheet.top + 262.0f, right, sheet.top + 322.0f),
                                 Theme::kDangerText, HAlign::Left, VAlign::Middle);
        }
        const std::wstring copy = Tr(Text::UsbCopyToXbox);
        const std::wstring play = Tr(Text::UsbPlayFromDrive);
        const float copy_width = PillWidth(renderer_, copy, false);
        const float play_width = PillWidth(renderer_, play, false);
        const float top = sheet.top + 350.0f;
        DrawChoicePill(renderer_, RectF(left, top, left + copy_width, top + kPillHeight), copy,
                       true, choice_focus_ == 0);
        const float play_left = left + copy_width + kPillGap;
        DrawChoicePill(renderer_, RectF(play_left, top, play_left + play_width, top + kPillHeight),
                       play, false, choice_focus_ == 1);
    }

    // The batch in progress: the file, a bar, the percentage, the rate and the bytes.
    void DrawProgress() {
        std::wstring name;
        {
            const std::lock_guard lock(mutex_);
            name = job_name_;
        }
        const bool moving = moving_.load();
        const std::uint64_t done = bytes_done_.load();
        const std::uint64_t size = bytes_size_.load();
        const double fraction = size > 0 ? static_cast<double>(done) / static_cast<double>(size)
                                         : 0.0;
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kWorkTop, kSheetLeft + kSheetWidth, kWorkTop + kWorkHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(name, Font::Heading,
                             RectF(left, sheet.top + 36.0f, right, sheet.top + 100.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
        renderer_.DrawString(std::wstring(moving ? Tr(Text::UsbMoving) : Tr(Text::UsbCopying)) +
                                 L"  " + std::to_wstring(job_index_.load()) + L" / " +
                                 std::to_wstring(job_total_.load()),
                             Font::Meta, RectF(left, sheet.top + 112.0f, right, sheet.top + 146.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        DrawProgressBar(renderer_, RectF(left, sheet.top + 176.0f, right, sheet.top + 192.0f),
                        fraction);
        if (!moving && size > 0) {
            const std::wstring stats =
                std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%   " +
                FormatBytes(static_cast<std::uint64_t>(rate_.load())) + L"/s   " +
                FormatBytes(done) + L" / " + FormatBytes(size);
            renderer_.DrawString(stats, Font::MetaMono,
                                 RectF(left, sheet.top + 214.0f, right, sheet.top + 254.0f),
                                 Theme::kText, HAlign::Left, VAlign::Middle);
        }
    }

    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    std::vector<Hint> CurrentHints() const {
        if (phase_ == Phase::Working) {
            return {{Theme::kButtonB, L"B", Tr(Text::HintCancel)}};
        }
        if (phase_ == Phase::Scanning) {
            return {{Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        if (choice_open_) {
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        std::vector<Hint> hints;
        if (RowCount() > 0) {
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
        }
        hints.push_back({Theme::kButtonX, L"X", Tr(Text::HintScan)});
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
        return hints;
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    fs::path local_state_;

    bool detected_ = false;
    UsbMode mode_ = UsbMode::Unset;
    UsbMode action_ = UsbMode::Unset;
    std::wstring result_;
    Phase phase_ = Phase::Scanning;
    std::vector<Item> items_;
    std::vector<Drive> drives_;
    std::optional<std::uint64_t> xbox_free_;
    int selected_ = 0;
    int top_ = 0;
    Tween scroll_;
    bool focus_all_ = false;
    bool choice_open_ = false;
    int choice_focus_ = 0; // 0 copy, 1 play from the drive
    std::wstring choice_message_;
    bool single_move_ = false;

    std::wstring toast_;
    Clock::time_point toast_until_{};

    bool closed_ = false;
    bool leaving_ = false;

    // Shared with the worker thread.
    std::thread worker_;
    std::atomic<bool> done_{true};
    std::atomic<bool> cancel_{false};
    std::mutex mutex_; // guards scan_ and job_name_
    std::optional<ScanResult> scan_;
    std::wstring job_name_;
    std::atomic<bool> moving_{false};
    std::atomic<int> job_index_{0};
    std::atomic<int> job_total_{0};
    std::atomic<std::uint64_t> bytes_done_{0};
    std::atomic<std::uint64_t> bytes_size_{0};
    std::atomic<double> rate_{0.0};
    std::atomic<int> ok_{0};
    std::atomic<int> failed_{0};
    std::atomic<int> skipped_{0};
    std::atomic<bool> cancelled_{false};
};

} // namespace

UsbMode LoadUsbMode(const fs::path& local_state) {
    std::ifstream file(local_state / "usb_mode.txt");
    std::string value;
    file >> value;
    for (const auto mode : {UsbMode::Ask, UsbMode::Copy, UsbMode::External, UsbMode::Off}) {
        if (value == ModeName(mode))
            return mode;
    }
    return UsbMode::Unset;
}

bool SaveUsbMode(const fs::path& local_state, UsbMode mode) {
    std::ofstream file(local_state / "usb_mode.txt", std::ios::trunc);
    file << ModeName(mode) << '\n';
    file.close();
    if (!file)
        Diagnostic("USB_DETECT failed saving mode");
    return static_cast<bool>(file);
}

const wchar_t* UsbModeLabel(UsbMode mode) {
    switch (mode) {
    case UsbMode::Ask:
        return Tr(Text::UsbModeAsk);
    case UsbMode::Copy:
        return Tr(Text::UsbModeCopy);
    case UsbMode::External:
        return Tr(Text::UsbModeExternal);
    case UsbMode::Off:
        return Tr(Text::UsbModeOff);
    default:
        return Tr(Text::UsbModeUnset);
    }
}

struct UsbDetection::State {
    fs::path local_state;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool running = false;
    Clock::time_point next{};
    std::set<std::wstring> connected;
    ScanResult result;
};

UsbDetection::UsbDetection(const fs::path& local_state) : state_(std::make_shared<State>()) {
    state_->local_state = local_state;
    Ready();
}

UsbDetection::~UsbDetection() {
    state_->cancel.store(true);
}

bool UsbDetection::Ready() {
    auto& state = *state_;
    if (state.running) {
        if (!state.done.load())
            return false;
        state.running = false;
        state.next = Clock::now() + std::chrono::seconds(5);
    }
    if (!state.result.drives.empty())
        return true;
    if (Clock::now() < state.next)
        return false;
    state.done.store(false);
    state.running = true;
    // The worker owns its state. Leaving the library never joins a pending storage request.
    std::thread([state = state_] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT {
                winrt::uninit_apartment();
            };
            if (LoadUsbMode(state->local_state) != UsbMode::Off) {
                state->result = ScanDrives(state->local_state, state->cancel, &state->connected);
            }
        } catch (const std::exception& error) {
            Diagnostic(std::string("USB_DETECT worker failed ") + error.what());
        } catch (...) {
            Diagnostic("USB_DETECT worker failed");
        }
        state->done.store(true);
    }).detach();
    return false;
}

bool UsbDetection::Run(Renderer& renderer, const CoreWindow& window, Input& input) {
    auto result = std::move(state_->result);
    state_->result = {};
    std::set<std::string> seen;
    std::ifstream file(state_->local_state / "usb_seen.txt");
    std::string identity;
    while (file >> std::quoted(identity))
        seen.insert(identity);
    for (auto& drive : result.drives) {
        const auto mode = LoadUsbMode(state_->local_state);
        if (mode == UsbMode::Off)
            break;
        drive.unseen = !seen.contains(DriveIdentity(drive));
        std::vector<Item> items;
        int games = 0;
        int keys = 0;
        for (const auto& item : result.items) {
            if (item.drive != drive.root)
                continue;
            items.push_back(item);
            if (!InDriveLibrary(item)) {
                if (item.is_key)
                    ++keys;
                else
                    ++games;
            }
        }
        Diagnostic("USB_DETECT drive=" + Utf8(drive.name) + " mode=" + ModeName(mode) +
                   " games=" + std::to_string(games) + " keys=" + std::to_string(keys) +
                   " new=" + (drive.unseen ? "yes" : "no"));
        try {
            UsbImportScreen screen(renderer, window, input, state_->local_state, std::move(drive),
                                   std::move(items), mode);
            if (screen.Run())
                return true;
        } catch (const winrt::hresult_error& error) {
            Diagnostic("USB_DETECT screen failed " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            Diagnostic(std::string("USB_DETECT screen failed ") + error.what());
        }
    }
    return false;
}

bool RunUsbImportScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                        const std::filesystem::path& local_state) {
    try {
        UsbImportScreen screen(renderer, window, input, local_state);
        return screen.Run();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI usb import screen failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI usb import screen failed ") + error.what());
    }
    return false;
}

} // namespace EdenXbox::Ui
