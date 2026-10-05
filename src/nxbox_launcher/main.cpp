// SPDX-License-Identifier: GPL-3.0-or-later

// Per-game launcher tile: opens nxbox://play?title=<ID> and exits. The ID comes from title.txt in
// the package install folder, so the same binary serves every game tile.

#include <chrono>
#include <fstream>
#include <memory>
#include <string>

#include <windows.h> // OutputDebugStringA + ::Sleep (sets the target-arch macros winnt.h needs)

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

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

// Keep the package's splash art on the CoreWindow until protocol acceptance. No downloads,
// metadata scan, or deliberate delay belongs in this process.
class Splash {
public:
    explicit Splash(const CoreWindow& window) {
        using Microsoft::WRL::ComPtr;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                        D3D11_SDK_VERSION, &device_, nullptr, &device_context_));
        ComPtr<IDXGIDevice> device;
        check_hresult(device_.As(&device));
        ComPtr<IDXGIAdapter> adapter;
        check_hresult(device->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 1920;
        desc.Height = 1080;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        check_hresult(factory->CreateSwapChainForCoreWindow(device_.Get(), get_unknown(window),
                                                            &desc, nullptr, &swap_));
        ComPtr<ID2D1Factory1> d2d_factory;
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                        reinterpret_cast<void**>(d2d_factory.GetAddressOf())));
        ComPtr<ID2D1Device> d2d_device;
        check_hresult(d2d_factory->CreateDevice(device.Get(), &d2d_device));
        ComPtr<ID2D1DeviceContext> context;
        check_hresult(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        ComPtr<IDXGISurface> surface;
        check_hresult(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        ComPtr<ID2D1Bitmap1> target;
        check_hresult(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target));
        context->SetTarget(target.Get());
        ComPtr<IWICImagingFactory2> wic;
        check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic)));
        const std::wstring file =
            std::wstring(
                Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()) +
            L"\\Assets\\SplashScreen.png";
        ComPtr<IWICBitmapDecoder> decoder;
        check_hresult(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder));
        ComPtr<IWICBitmapFrameDecode> frame;
        check_hresult(decoder->GetFrame(0, &frame));
        ComPtr<IWICFormatConverter> converter;
        check_hresult(wic->CreateFormatConverter(&converter));
        check_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                            WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom));
        ComPtr<ID2D1Bitmap1> bitmap;
        check_hresult(context->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
        context->BeginDraw();
        context->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        // Preserve the asset's aspect ratio instead of stretching the game art.
        const auto size = bitmap->GetSize();
        const float height = 1920.0f * size.height / size.width;
        context->DrawBitmap(bitmap.Get(),
                            D2D1::RectF(0, (1080 - height) / 2, 1920, (1080 + height) / 2), 1.0f,
                            D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
        check_hresult(context->EndDraw());
        const DXGI_PRESENT_PARAMETERS parameters{};
        check_hresult(swap_->Present1(1, 0, &parameters));
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
};

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
        std::unique_ptr<Splash> splash;
        try {
            splash = std::make_unique<Splash>(window);
        } catch (const winrt::hresult_error& error) {
            Log("splash unavailable: " + winrt::to_string(error.message()));
        }
        window.Activate();

        const std::string id = ReadTitleId();
        if (id.empty()) {
            Log("title.txt is missing or is not 16 hex digits");
            return;
        }
        Log("launching nxbox://play?title=" + id);

        try {
            Windows::Foundation::Uri uri{winrt::to_hstring("nxbox://play?title=" + id)};
            auto operation = Windows::System::Launcher::LaunchUriAsync(uri);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (operation.Status() == Windows::Foundation::AsyncStatus::Started &&
                   std::chrono::steady_clock::now() < deadline) {
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                ::Sleep(1);
            }
            if (operation.Status() == Windows::Foundation::AsyncStatus::Started) {
                operation.Cancel();
                Log("protocol acceptance timed out");
            } else {
                Log(operation.GetResults() ? "protocol accepted; exiting" : "NXbox unavailable");
            }
        } catch (const winrt::hresult_error& error) {
            Log("launch failed: " + winrt::to_string(error.message()));
        }
        splash.reset();
        CoreApplication::Exit();
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<LauncherView>());
    return 0;
}
