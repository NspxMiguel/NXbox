// SPDX-License-Identifier: GPL-3.0-or-later

// Per-game launcher tile: opens nxbox://play?title=<ID> and exits. The ID comes from title.txt in
// the package install folder, so the same binary serves every game tile.

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>

#include <windows.h> // OutputDebugStringA + ::Sleep (sets the target-arch macros winnt.h needs)

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {

void Log(const std::string& message) {
    OutputDebugStringA(("[nxbox-launcher] " + message + "\n").c_str());
}

// The 16 hex digits of title.txt, or an empty string when the file is missing or malformed.
std::string ReadTitleId() {
    const std::string path =
        winrt::to_string(Windows::ApplicationModel::Package::Current().InstalledLocation().Path()) +
        "\\title.txt";
    std::ifstream in(path);
    std::string id;
    std::getline(in, id);
    while (!id.empty() && (id.back() == '\r' || id.back() == ' ' || id.back() == '\n')) {
        id.pop_back();
    }
    if (id.size() != 16) {
        return {};
    }
    for (const char c : id) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return {};
        }
    }
    return id;
}

struct LauncherView : implements<LauncherView, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() {
        return *this;
    }
    void Initialize(CoreApplicationView const&) {}
    void SetWindow(CoreWindow const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}

    void Run() {
        // A UWP app must activate its CoreWindow and pump the dispatcher, or the system ends it.
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();

        const std::string id = ReadTitleId();
        if (id.empty()) {
            Log("title.txt is missing or is not 16 hex digits");
            return;
        }
        Log("launching nxbox://play?title=" + id);

        std::atomic<bool> done{false};
        Windows::Foundation::Uri uri{winrt::to_hstring("nxbox://play?title=" + id)};
        auto operation = Windows::System::Launcher::LaunchUriAsync(uri);
        operation.Completed([&done](Windows::Foundation::IAsyncOperation<bool> const& result,
                                    Windows::Foundation::AsyncStatus) {
            try {
                Log(result.GetResults() ? "NXbox opened" : "the system did not open NXbox");
            } catch (winrt::hresult_error const& error) {
                Log("launch failed: " + winrt::to_string(error.message()));
            }
            done.store(true);
        });

        // Bounded, so a launch that never completes cannot leave a stuck tile.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            ::Sleep(20);
        }
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<LauncherView>());
    return 0;
}
