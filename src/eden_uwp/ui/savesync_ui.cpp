// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/savesync_ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <winrt/Windows.Foundation.h>

#include "common/scope_exit.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/save_sync.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;
using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

constexpr float kMargin = kScreenMargin;
constexpr float kTextWidth = 1100.0f;
constexpr float kTitleTop = 170.0f;
constexpr float kTitleBottom = 430.0f;
constexpr float kBodyTop = 462.0f;
constexpr float kBodyBottom = 660.0f;
constexpr float kChoiceTop = 730.0f;
constexpr float kBarWidth = 900.0f;
constexpr float kBarHeight = 14.0f;

constexpr auto kResultHold = std::chrono::milliseconds(3500);
constexpr auto kQuickHold = std::chrono::milliseconds(1200);

// Gives a worker thread a COM apartment of its own for the WinRT calls the backend makes.
class ScopedApartment {
public:
    ScopedApartment() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    ~ScopedApartment() {
        winrt::uninit_apartment();
    }
    ScopedApartment(const ScopedApartment&) = delete;
    ScopedApartment& operator=(const ScopedApartment&) = delete;
};

// Runs the frames of one screen: dispatcher, input, a frame of drawing. `frame` returns false when
// the screen is done. Returns true when the window was closed instead.
bool RunFrames(Renderer& renderer, const CoreWindow& window, Input& input,
               const std::function<bool(Clock::time_point)>& frame) {
    bool closed = false;
    const auto token = window.Closed([&closed](const auto&, const auto&) { closed = true; });
    SCOPE_EXIT {
        window.Closed(token);
    };
    while (!closed) {
        const Clock::time_point frame_start = Clock::now();
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        input.Update();
        const Clock::time_point now = Clock::now();
        renderer.BeginFrame();
        const bool keep = frame(now);
        renderer.EndFrame();
        if (!keep) {
            break;
        }
        // Present1(1, 0) paces the loop on the display; only an early return needs a sleep.
        const Clock::duration spent = Clock::now() - frame_start;
        if (spent < std::chrono::milliseconds(8)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
        }
    }
    return closed;
}

void DrawBackground(Renderer& renderer) {
    const D2D1_GRADIENT_STOP glow[2] = {{0.0f, Theme::Rgb(0x26262B, 0.9f)},
                                        {1.0f, Theme::Rgb(0x26262B, 0.0f)}};
    renderer.FillGradient(RectF(0.0f, 0.0f, kCanvasWidth, 620.0f), 0.0f, glow, 2,
                          Point2F(0.0f, 0.0f), Point2F(0.0f, 620.0f));
}

void DrawTitle(Renderer& renderer, const std::wstring& title) {
    const Font font = renderer.MeasureString(title, Font::Title, kTextWidth).lines > 2
                          ? Font::TitleSmall
                          : Font::Title;
    renderer.DrawString(title, font, RectF(kMargin, kTitleTop, kMargin + kTextWidth, kTitleBottom),
                        Theme::kText, HAlign::Left, VAlign::Bottom);
}

void DrawBody(Renderer& renderer, const std::wstring& body) {
    renderer.DrawString(body, Font::Body, RectF(kMargin, kBodyTop, kMargin + 1000.0f, kBodyBottom),
                        Theme::kTextSecondary);
}

std::wstring FormatWhen(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) {
        return L"-";
    }
    const std::time_t stamp = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    if (localtime_s(&local, &stamp) != 0) {
        return L"-";
    }
    wchar_t buffer[40];
    if (CurrentLanguage() == Language::Portuguese) {
        swprintf_s(buffer, std::size(buffer), L"%02d/%02d/%04d %02d:%02d", local.tm_mday,
                   local.tm_mon + 1, local.tm_year + 1900, local.tm_hour, local.tm_min);
    } else {
        swprintf_s(buffer, std::size(buffer), L"%02d/%02d/%04d %02d:%02d", local.tm_mon + 1,
                   local.tm_mday, local.tm_year + 1900, local.tm_hour, local.tm_min);
    }
    return buffer;
}

fs::path PendingFile(const fs::path& local_state) {
    return local_state / "savesync_pending.txt";
}

const char* StatusName(SaveSync::SyncStatus status) {
    switch (status) {
    case SaveSync::SyncStatus::Failed:
        return "failed";
    case SaveSync::SyncStatus::NotConfigured:
        return "not-configured";
    case SaveSync::SyncStatus::NotSignedIn:
        return "not-signed-in";
    case SaveSync::SyncStatus::NothingToSync:
        return "nothing";
    case SaveSync::SyncStatus::UpToDate:
        return "up-to-date";
    case SaveSync::SyncStatus::Uploaded:
        return "uploaded";
    case SaveSync::SyncStatus::Downloaded:
        return "downloaded";
    case SaveSync::SyncStatus::Conflict:
        return "conflict";
    }
    return "?";
}

std::uint64_t ParseTitleId(const std::string& hex) {
    return std::strtoull(hex.c_str(), nullptr, 16);
}

const wchar_t* StageText(SaveSync::SyncStage stage) {
    switch (stage) {
    case SaveSync::SyncStage::Connecting:
        return Tr(Text::SyncStageConnecting);
    case SaveSync::SyncStage::Comparing:
        return Tr(Text::SyncStageComparing);
    case SaveSync::SyncStage::Downloading:
        return Tr(Text::SyncStageDownloading);
    case SaveSync::SyncStage::Applying:
        return Tr(Text::SyncStageApplying);
    case SaveSync::SyncStage::Uploading:
        return Tr(Text::SyncStageUploading);
    case SaveSync::SyncStage::Cleaning:
        return Tr(Text::SyncStageCleaning);
    case SaveSync::SyncStage::Finished:
        return Tr(Text::SyncStageFinished);
    }
    return L"";
}

// A title and a paragraph, until A. For the states that only need to be read.
bool RunMessage(Renderer& renderer, const CoreWindow& window, Input& input,
                const std::wstring& title, const std::wstring& body) {
    return RunFrames(renderer, window, input, [&](Clock::time_point) {
        if (input.Pressed(Button::A) || input.Pressed(Button::B)) {
            return false;
        }
        DrawBackground(renderer);
        DrawTitle(renderer, title);
        DrawBody(renderer, body);
        DrawHints(renderer, {{Theme::kButtonA, L"A", Tr(Text::HintContinue)}});
        return true;
    });
}

// ---------------------------------------------------------------------------------------------
// Sign-in
// ---------------------------------------------------------------------------------------------

// Starts the device login and polls it on a thread of its own, so the screen keeps drawing.
class SignInRunner {
public:
    SignInRunner() {
        worker_ = std::thread([this] { Run(); });
    }
    ~SignInRunner() {
        cancelled_.store(true);
        SaveSync::CancelDeviceLogin();
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    SignInRunner(const SignInRunner&) = delete;
    SignInRunner& operator=(const SignInRunner&) = delete;

    void Cancel() {
        cancelled_.store(true);
        SaveSync::CancelDeviceLogin();
    }
    bool Done() const {
        return done_.load();
    }
    bool HasCode() const {
        return has_code_.load();
    }
    SaveSync::DeviceLogin Login() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return login_;
    }
    SignInOutcome Outcome() const {
        return static_cast<SignInOutcome>(outcome_.load());
    }

private:
    void Run() {
        try {
            const ScopedApartment apartment;
            SaveSync::DeviceLogin login = SaveSync::StartDeviceLogin();
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                login_ = login;
            }
            if (!login.ok) {
                Diagnostic("UI savesync sign-in could not start: " + login.error);
                outcome_.store(static_cast<int>(SignInOutcome::Failed));
            } else if (cancelled_.load()) {
                outcome_.store(static_cast<int>(SignInOutcome::Cancelled));
            } else {
                has_code_.store(true);
                Diagnostic("UI savesync sign-in code shown");
                switch (SaveSync::PollDeviceLogin()) {
                case SaveSync::LoginResult::SignedIn:
                    outcome_.store(static_cast<int>(SignInOutcome::SignedIn));
                    break;
                case SaveSync::LoginResult::Denied:
                    outcome_.store(static_cast<int>(SignInOutcome::Denied));
                    break;
                case SaveSync::LoginResult::Expired:
                    outcome_.store(static_cast<int>(SignInOutcome::Expired));
                    break;
                case SaveSync::LoginResult::Cancelled:
                    outcome_.store(static_cast<int>(SignInOutcome::Cancelled));
                    break;
                case SaveSync::LoginResult::Failed:
                default:
                    outcome_.store(static_cast<int>(SignInOutcome::Failed));
                    break;
                }
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI savesync sign-in failed " + winrt::to_string(error.message()));
            outcome_.store(static_cast<int>(SignInOutcome::Failed));
        } catch (const std::exception& error) {
            Diagnostic(std::string("UI savesync sign-in failed ") + error.what());
            outcome_.store(static_cast<int>(SignInOutcome::Failed));
        }
        done_.store(true);
    }

    std::thread worker_;
    mutable std::mutex mutex_;
    SaveSync::DeviceLogin login_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> has_code_{false};
    std::atomic<bool> done_{false};
    std::atomic<int> outcome_{static_cast<int>(SignInOutcome::Failed)};
};

const wchar_t* OutcomeText(SignInOutcome outcome) {
    switch (outcome) {
    case SignInOutcome::SignedIn:
        return Tr(Text::SyncSignedIn);
    case SignInOutcome::Denied:
        return Tr(Text::SyncDenied);
    case SignInOutcome::Expired:
        return Tr(Text::SyncExpired);
    case SignInOutcome::NotConfigured:
        return Tr(Text::SyncNotConfiguredBody);
    case SignInOutcome::Cancelled:
        return L"";
    case SignInOutcome::Failed:
    default:
        return Tr(Text::SyncLoginFailed);
    }
}

// ---------------------------------------------------------------------------------------------
// Saves
// ---------------------------------------------------------------------------------------------

enum class FlowMode { BeforeBoot, AfterExit };

// One backend call on a thread of its own, with the progress it reports.
class SyncRunner {
public:
    SyncRunner() = default;
    ~SyncRunner() {
        Join();
    }
    SyncRunner(const SyncRunner&) = delete;
    SyncRunner& operator=(const SyncRunner&) = delete;

    void Start(std::function<SaveSync::SyncResult(const SaveSync::ProgressCallback&)> operation) {
        Join();
        done_.store(false);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            progress_ = SaveSync::SyncProgress{};
            result_ = SaveSync::SyncResult{};
        }
        worker_ = std::thread([this, operation = std::move(operation)] {
            SaveSync::SyncResult result;
            try {
                const ScopedApartment apartment;
                result = operation([this](const SaveSync::SyncProgress& progress) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    progress_ = progress;
                });
            } catch (const winrt::hresult_error& error) {
                result.status = SaveSync::SyncStatus::Failed;
                result.message = winrt::to_string(error.message());
            } catch (const std::exception& error) {
                result.status = SaveSync::SyncStatus::Failed;
                result.message = error.what();
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                result_ = std::move(result);
            }
            done_.store(true);
        });
    }

    bool Done() const {
        return done_.load();
    }
    SaveSync::SyncProgress Progress() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return progress_;
    }
    SaveSync::SyncResult Result() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return result_;
    }

private:
    void Join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::thread worker_;
    mutable std::mutex mutex_;
    SaveSync::SyncProgress progress_;
    SaveSync::SyncResult result_;
    std::atomic<bool> done_{true};
};

// What a finished sync tells the player, or null when it needs no words.
const wchar_t* ResultText(SaveSync::SyncStatus status) {
    switch (status) {
    case SaveSync::SyncStatus::Uploaded:
        return Tr(Text::SyncResultUploaded);
    case SaveSync::SyncStatus::Downloaded:
        return Tr(Text::SyncResultDownloaded);
    case SaveSync::SyncStatus::Failed:
        return Tr(Text::SyncResultFailed);
    case SaveSync::SyncStatus::NotSignedIn:
        return Tr(Text::SyncResultNotSignedIn);
    case SaveSync::SyncStatus::NotConfigured:
        return Tr(Text::SyncResultNotConfigured);
    case SaveSync::SyncStatus::UpToDate:
    case SaveSync::SyncStatus::NothingToSync:
    case SaveSync::SyncStatus::Conflict:
    default:
        return nullptr;
    }
}

class SyncFlow {
public:
    SyncFlow(Renderer& renderer, const CoreWindow& window, Input& input, const SyncGame& game,
             FlowMode mode)
        : renderer_(renderer), window_(window), input_(input), game_(game), mode_(mode) {}

    struct Outcome {
        bool window_closed = false;
        SaveSync::SyncStatus status = SaveSync::SyncStatus::Failed;
        bool conflict_left = false; // the conflict was not answered
    };

    Outcome Run() {
        const std::uint64_t title = ParseTitleId(game_.title_id);
        const std::string name = game_.name;
        Diagnostic(std::string("UI savesync ") +
                   (mode_ == FlowMode::BeforeBoot ? "before-boot " : "after-exit ") +
                   game_.title_id + " begin");
        runner_.Start([title, name, this](const SaveSync::ProgressCallback& progress) {
            return mode_ == FlowMode::BeforeBoot ? SaveSync::SyncBeforeBoot(title, progress, name)
                                                 : SaveSync::SyncAfterExit(title, progress, name);
        });
        Outcome outcome;
        outcome.window_closed = RunFrames(renderer_, window_, input_, [&](Clock::time_point now) {
            return Frame(now, title, name, outcome);
        });
        Diagnostic(std::string("UI savesync ") + game_.title_id + " end " +
                   StatusName(outcome.status));
        return outcome;
    }

private:
    enum class Phase { Syncing, Conflict, Result };

    bool Frame(Clock::time_point now, std::uint64_t title, const std::string& name,
               Outcome& outcome) {
        DrawBackground(renderer_);
        if (phase_ == Phase::Syncing && runner_.Done()) {
            if (OnFinished(now, outcome)) {
                return false; // nothing to say: go on
            }
        }
        switch (phase_) {
        case Phase::Syncing:
            DrawProgress();
            return true;
        case Phase::Conflict:
            return DrawConflict(title, name, outcome);
        case Phase::Result:
            return DrawResult(now);
        }
        return false;
    }

    // A backend call finished. Returns true when the screen is done, false when it has to show a
    // conflict or a message first.
    bool OnFinished(Clock::time_point now, Outcome& outcome) {
        const SaveSync::SyncResult result = runner_.Result();
        Diagnostic(fmt::format("UI savesync {} -> {} {}", game_.title_id, StatusName(result.status),
                               result.message));
        outcome.status = result.status;
        if (result.status == SaveSync::SyncStatus::Conflict) {
            conflict_ = result;
            focus_ = result.cloud_modified > result.local_modified ? 1 : 0;
            phase_ = Phase::Conflict;
            outcome.conflict_left = true;
            return false;
        }
        outcome.conflict_left = false;
        const wchar_t* text = ResultText(result.status);
        if (text == nullptr) {
            return true;
        }
        result_text_ = text;
        result_is_problem_ = result.status == SaveSync::SyncStatus::Failed ||
                             result.status == SaveSync::SyncStatus::NotSignedIn ||
                             result.status == SaveSync::SyncStatus::NotConfigured;
        result_until_ = now + (result_is_problem_ ? kResultHold : kQuickHold);
        phase_ = Phase::Result;
        return false;
    }

    bool DrawResult(Clock::time_point now) {
        if (now >= result_until_ || input_.Pressed(Button::A) || input_.Pressed(Button::B)) {
            return false;
        }
        DrawTitle(renderer_, mode_ == FlowMode::BeforeBoot ? Tr(Text::SyncingTitle)
                                                           : Tr(Text::SyncingAfterTitle));
        renderer_.DrawString(result_text_, Font::Body,
                             RectF(kMargin, kBodyTop, kMargin + 1000.0f, kBodyBottom),
                             result_is_problem_ ? Theme::kDangerText : Theme::kSuccessText);
        DrawHints(renderer_, {{Theme::kButtonA, L"A", Tr(Text::HintContinue)}});
        return true;
    }

    void DrawProgress() {
        const SaveSync::SyncProgress progress = runner_.Progress();
        renderer_.DrawString(game_.display_name, Font::Nav,
                             RectF(kMargin, kTitleTop, kMargin + kTextWidth, kTitleTop + 44.0f),
                             Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        DrawTitle(renderer_, mode_ == FlowMode::BeforeBoot ? Tr(Text::SyncingTitle)
                                                           : Tr(Text::SyncingAfterTitle));
        renderer_.DrawString(StageText(progress.stage), Font::Body,
                             RectF(kMargin, kBodyTop, kMargin + 1000.0f, kBodyTop + 40.0f),
                             Theme::kTextSecondary);
        const D2D1_RECT_F track =
            RectF(kMargin, kBodyTop + 64.0f, kMargin + kBarWidth, kBodyTop + 64.0f + kBarHeight);
        double fraction = 0.0;
        if (progress.total > 0) {
            fraction = static_cast<double>(progress.done) / static_cast<double>(progress.total);
        } else {
            // The size is not known yet: a slow swell instead of a fraction.
            const double seconds =
                std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            fraction = 0.12 + 0.08 * std::sin(seconds * 3.0);
        }
        DrawProgressBar(renderer_, track, fraction);
        if (progress.total > 0) {
            renderer_.DrawString(std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%",
                                 Font::MetaStrong,
                                 RectF(track.right + 24.0f, track.top - 12.0f, track.right + 200.0f,
                                       track.bottom + 12.0f),
                                 Theme::kText, HAlign::Left, VAlign::Middle);
        }
        if (!progress.item.empty()) {
            renderer_.DrawString(Widen(progress.item), Font::MetaMono,
                                 RectF(kMargin, track.bottom + 24.0f, kMargin + kBarWidth,
                                       track.bottom + 56.0f),
                                 Theme::kTextTertiary);
        }
    }

    // One side of a conflict: its newest file time, file count and size, as text.
    void DrawSide(float x, const wchar_t* label, std::int64_t when, std::uint32_t files,
                  std::uint64_t bytes) {
        renderer_.DrawString(label, Font::Section, RectF(x, 580.0f, x + 480.0f, 614.0f),
                             Theme::kText);
        renderer_.DrawString(FormatWhen(when), Font::MetaMono,
                             RectF(x, 624.0f, x + 480.0f, 656.0f), Theme::kTextSecondary);
        renderer_.DrawString(std::to_wstring(files) + L" " + Tr(Text::SyncFilesSuffix) + L"  ·  " +
                                 FormatBytes(bytes),
                             Font::MetaMono, RectF(x, 660.0f, x + 480.0f, 692.0f),
                             Theme::kTextSecondary);
    }

    bool DrawConflict(std::uint64_t title, const std::string& name, Outcome& outcome) {
        if (input_.Pressed(Button::Left)) {
            focus_ = 0;
        }
        if (input_.Pressed(Button::Right)) {
            focus_ = 1;
        }
        if (input_.Pressed(Button::B)) {
            Diagnostic("UI savesync conflict left unresolved " + game_.title_id);
            outcome.conflict_left = true;
            return false;
        }
        if (input_.Pressed(Button::A)) {
            const SaveSync::ConflictChoice choice = focus_ == 0 ? SaveSync::ConflictChoice::KeepXbox
                                                                : SaveSync::ConflictChoice::KeepCloud;
            Diagnostic(std::string("UI savesync conflict answer ") +
                       (focus_ == 0 ? "keep-xbox" : "keep-cloud"));
            runner_.Start([title, name, choice](const SaveSync::ProgressCallback& progress) {
                return SaveSync::ResolveConflict(title, choice, progress, name);
            });
            phase_ = Phase::Syncing;
            return true;
        }
        DrawTitle(renderer_, Tr(Text::SyncConflictTitle));
        renderer_.DrawString(Tr(Text::SyncConflictBody), Font::Body,
                             RectF(kMargin, 450.0f, kMargin + 1000.0f, 566.0f),
                             Theme::kTextSecondary);
        DrawSide(kMargin, Tr(Text::SyncSideXbox), conflict_.local_modified, conflict_.local_files,
                 conflict_.local_bytes);
        DrawSide(kMargin + 560.0f, Tr(Text::SyncSideCloud), conflict_.cloud_modified,
                 conflict_.cloud_files, conflict_.cloud_bytes);
        const std::wstring keep_xbox = Tr(Text::SyncKeepXbox);
        const std::wstring keep_cloud = Tr(Text::SyncKeepCloud);
        const float first = renderer_.MeasureString(keep_xbox, Font::Button).width +
                            2.0f * kPillPadding;
        const float second = renderer_.MeasureString(keep_cloud, Font::Button).width +
                             2.0f * kPillPadding;
        DrawChoicePill(renderer_, RectF(kMargin, kChoiceTop, kMargin + first, kChoiceTop + kPillHeight),
                       keep_xbox, true, focus_ == 0);
        const float left = kMargin + first + kPillGap;
        DrawChoicePill(renderer_, RectF(left, kChoiceTop, left + second, kChoiceTop + kPillHeight),
                       keep_cloud, false, focus_ == 1);
        DrawHints(renderer_, {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                              {Theme::kButtonB, L"B", Tr(Text::SyncSkip)}});
        return true;
    }

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    SyncGame game_;
    FlowMode mode_;
    SyncRunner runner_;
    Phase phase_ = Phase::Syncing;
    SaveSync::SyncResult conflict_;
    int focus_ = 0;
    std::wstring result_text_;
    bool result_is_problem_ = false;
    Clock::time_point result_until_{};
};

// A renderer and an input for the screens that run on their own, released when `body` returns.
// Returns true when the window was closed.
bool WithOwnRenderer(const CoreWindow& window, const char* what,
                     const std::function<bool(Renderer&, Input&)>& body) {
    bool closed = false;
    try {
        Renderer renderer;
        renderer.Initialize(window);
        {
            Input input(window);
            closed = body(renderer, input);
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic(std::string("UI ") + what + " failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI ") + what + " failed " + error.what());
    }
    return closed;
}

void WritePending(const fs::path& local_state, const SyncGame& game) {
    std::ofstream out(PendingFile(local_state), std::ios::trunc);
    out << game.title_id << '\n' << game.name << '\n';
}

void ClearPending(const fs::path& local_state) {
    std::error_code ec;
    fs::remove(PendingFile(local_state), ec);
}

bool ReadPending(const fs::path& local_state, SyncGame& game) {
    std::ifstream in(PendingFile(local_state));
    std::string title;
    std::string name;
    if (!std::getline(in, title) || title.size() < 16) {
        return false;
    }
    std::getline(in, name);
    while (!title.empty() && (title.back() == '\r' || title.back() == ' ')) {
        title.pop_back();
    }
    while (!name.empty() && (name.back() == '\r')) {
        name.pop_back();
    }
    game.title_id = title;
    game.name = name;
    game.display_name = Widen(name);
    return true;
}

} // namespace

SyncAccount GetSyncAccount() {
    if (!SaveSync::IsConfigured()) {
        return SyncAccount::NotConfigured;
    }
    return SaveSync::IsSignedIn() ? SyncAccount::SignedIn : SyncAccount::SignedOut;
}

bool SyncEnabled() {
    return GetSyncAccount() == SyncAccount::SignedIn;
}

bool IsSetupDone(const fs::path& local_state) {
    std::error_code ec;
    return fs::exists(local_state / "setup_done.txt", ec);
}

SignInOutcome RunSignIn(Renderer& renderer, const CoreWindow& window, Input& input,
                        bool& window_closed) {
    window_closed = false;
    if (!SaveSync::IsConfigured()) {
        Diagnostic("UI savesync sign-in: savesync.json is missing");
        window_closed = RunMessage(renderer, window, input, Tr(Text::SyncRowTitle),
                                   Tr(Text::SyncNotConfiguredBody));
        return SignInOutcome::NotConfigured;
    }
    Diagnostic("UI savesync sign-in begin");
    SignInRunner runner;
    bool finished = false;
    Clock::time_point shown_at{};
    Clock::time_point code_since{};
    bool code_seen = false;
    int expires_in = 0;
    window_closed = RunFrames(renderer, window, input, [&](Clock::time_point now) {
        DrawBackground(renderer);
        if (!finished && runner.Done()) {
            finished = true;
            shown_at = now;
            Diagnostic(fmt::format("UI savesync sign-in outcome {}",
                                   static_cast<int>(runner.Outcome())));
            if (runner.Outcome() == SignInOutcome::Cancelled) {
                return false;
            }
        }
        if (finished) {
            const SignInOutcome outcome = runner.Outcome();
            DrawTitle(renderer, Tr(Text::SyncRowTitle));
            renderer.DrawString(OutcomeText(outcome), Font::Body,
                                RectF(kMargin, kBodyTop, kMargin + 1000.0f, kBodyBottom),
                                outcome == SignInOutcome::SignedIn ? Theme::kSuccessText
                                                                   : Theme::kDangerText);
            DrawHints(renderer, {{Theme::kButtonA, L"A", Tr(Text::HintContinue)}});
            return !(input.Pressed(Button::A) || input.Pressed(Button::B) ||
                     now - shown_at >= kResultHold);
        }
        if (input.Pressed(Button::B)) {
            runner.Cancel();
            return true; // the runner finishes as Cancelled within a moment
        }
        DrawTitle(renderer, Tr(Text::SyncCodeTitle));
        if (runner.HasCode()) {
            const SaveSync::DeviceLogin login = runner.Login();
            if (!code_seen) {
                code_seen = true;
                code_since = now;
                expires_in = login.expires_in_seconds;
            }
            renderer.DrawString(Tr(Text::SyncCodeBody), Font::Body,
                                RectF(kMargin, kBodyTop - 20.0f, kMargin + 1000.0f, kBodyTop + 28.0f),
                                Theme::kTextSecondary);
            renderer.DrawString(Widen(login.verification_url), Font::Heading,
                                RectF(kMargin, kBodyTop + 40.0f, kMargin + kTextWidth,
                                      kBodyTop + 100.0f),
                                Theme::kText, HAlign::Left, VAlign::Middle);
            renderer.DrawString(Widen(login.user_code), Font::Code,
                                RectF(kMargin, kBodyTop + 110.0f, kMargin + kTextWidth,
                                      kBodyTop + 250.0f),
                                Theme::kText, HAlign::Left, VAlign::Middle);
            std::wstring waiting = Tr(Text::SyncWaiting);
            if (expires_in > 0) {
                const int left =
                    std::max(expires_in - static_cast<int>(std::chrono::duration_cast<
                                              std::chrono::seconds>(now - code_since).count()),
                             0);
                wchar_t clock[16];
                swprintf_s(clock, std::size(clock), L"%d:%02d", left / 60, left % 60);
                waiting += L"   " + std::wstring(Tr(Text::SyncExpiresIn)) + L" " + clock;
            }
            renderer.DrawString(waiting, Font::Meta,
                                RectF(kMargin, kBodyTop + 262.0f, kMargin + kTextWidth,
                                      kBodyTop + 300.0f),
                                Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
        } else {
            renderer.DrawString(Tr(Text::SyncStageConnecting), Font::Body,
                                RectF(kMargin, kBodyTop, kMargin + 1000.0f, kBodyTop + 40.0f),
                                Theme::kTextSecondary);
        }
        DrawHints(renderer, {{Theme::kButtonB, L"B", Tr(Text::HintCancel)}});
        return true;
    });
    return finished ? runner.Outcome() : SignInOutcome::Cancelled;
}

bool RunFirstRunSetup(Renderer& renderer, const CoreWindow& window, Input& input,
                      const fs::path& local_state) {
    Diagnostic("UI setup begin");
    int focus = 0; // 0 = Sim, 1 = Agora não
    bool wants_sync = false;
    bool decided = false;
    const bool closed = RunFrames(renderer, window, input, [&](Clock::time_point) {
        if (input.Pressed(Button::Left)) {
            focus = 0;
        }
        if (input.Pressed(Button::Right)) {
            focus = 1;
        }
        if (input.Pressed(Button::B)) {
            decided = true;
            wants_sync = false;
            return false;
        }
        if (input.Pressed(Button::A)) {
            decided = true;
            wants_sync = focus == 0;
            return false;
        }
        DrawBackground(renderer);
        DrawTitle(renderer, Tr(Text::SyncSetupTitle));
        DrawBody(renderer, Tr(Text::SyncSetupBody));
        const std::wstring yes = Tr(Text::SyncYes);
        const std::wstring no = Tr(Text::SyncNotNow);
        const float yes_width = renderer.MeasureString(yes, Font::Button).width + 2.0f * kPillPadding;
        const float no_width = renderer.MeasureString(no, Font::Button).width + 2.0f * kPillPadding;
        DrawChoicePill(renderer, RectF(kMargin, kChoiceTop, kMargin + yes_width, kChoiceTop + kPillHeight),
                       yes, true, focus == 0);
        const float left = kMargin + yes_width + kPillGap;
        DrawChoicePill(renderer, RectF(left, kChoiceTop, left + no_width, kChoiceTop + kPillHeight),
                       no, false, focus == 1);
        DrawHints(renderer, {{Theme::kButtonA, L"A", Tr(Text::HintSelect)}});
        return true;
    });
    if (closed || !decided) {
        return true;
    }
    Diagnostic(std::string("UI setup answer ") + (wants_sync ? "sync" : "no sync"));
    if (wants_sync) {
        bool sign_in_closed = false;
        const SignInOutcome outcome = RunSignIn(renderer, window, input, sign_in_closed);
        Diagnostic(fmt::format("UI setup sign-in outcome {}", static_cast<int>(outcome)));
        if (sign_in_closed) {
            return true;
        }
    }
    std::ofstream(local_state / "setup_done.txt", std::ios::trunc) << "1\n";
    Diagnostic("UI setup done");
    return false;
}

bool HasPendingSync(const fs::path& local_state) {
    SyncGame game;
    return ReadPending(local_state, game);
}

bool RunBootSync(const CoreWindow& window, const fs::path& local_state, const SyncGame& game) {
    if (!SyncEnabled()) {
        return false;
    }
    SyncFlow::Outcome outcome;
    const bool closed = WithOwnRenderer(window, "savesync before-boot", [&](Renderer& renderer, Input& input) {
        SyncFlow flow(renderer, window, input, game, FlowMode::BeforeBoot);
        outcome = flow.Run();
        return outcome.window_closed;
    });
    if (closed) {
        return true;
    }
    // An upload is owed from the moment the game can change the save, until it has been done.
    WritePending(local_state, game);
    return false;
}

bool RunPendingSync(const CoreWindow& window, const fs::path& local_state) {
    SyncGame game;
    if (!ReadPending(local_state, game)) {
        return false;
    }
    if (!SyncEnabled()) {
        Diagnostic("UI savesync pending dropped, sync is off");
        ClearPending(local_state);
        return false;
    }
    Diagnostic("UI savesync pending " + game.title_id);
    SyncFlow::Outcome outcome;
    const bool closed = WithOwnRenderer(window, "savesync pending", [&](Renderer& renderer, Input& input) {
        SyncFlow flow(renderer, window, input, game, FlowMode::AfterExit);
        outcome = flow.Run();
        return outcome.window_closed;
    });
    if (!closed && !outcome.conflict_left &&
        outcome.status != SaveSync::SyncStatus::Failed) {
        ClearPending(local_state);
    }
    return closed;
}

void SyncAfterExitHeadless(const fs::path& local_state, const SyncGame& game) {
    if (!SyncEnabled()) {
        ClearPending(local_state);
        return;
    }
    Diagnostic("UI savesync after-exit " + game.title_id + " begin");
    const SaveSync::SyncResult result =
        SaveSync::SyncAfterExit(ParseTitleId(game.title_id), {}, game.name);
    Diagnostic(fmt::format("UI savesync after-exit {} -> {} {}", game.title_id,
                           StatusName(result.status), result.message));
    // A conflict or a failure keeps the debt: the next launch finishes it with a screen.
    if (result.status != SaveSync::SyncStatus::Conflict &&
        result.status != SaveSync::SyncStatus::Failed) {
        ClearPending(local_state);
    }
}

} // namespace EdenXbox::Ui
