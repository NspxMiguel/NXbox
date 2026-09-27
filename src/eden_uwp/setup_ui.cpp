// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/setup_ui.h"

#include <chrono>
#include <thread>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

#include "eden_uwp/diagnostic.h"

namespace EdenXbox {
namespace {

using Microsoft::WRL::ComPtr;

// True black background, one accent color (Switch red), per docs/nxbox-ui.md's visual language —
// this first pass only proves the render pipeline end to end, not the real setup screens.
constexpr D2D1_COLOR_F kBackground = {0.0f, 0.0f, 0.0f, 1.0f};
constexpr D2D1_COLOR_F kAccent = {0xE6 / 255.0f, 0x00 / 255.0f, 0x12 / 255.0f, 1.0f};
constexpr D2D1_COLOR_F kText = {0xF2 / 255.0f, 0xF5 / 255.0f, 0xF4 / 255.0f, 1.0f};

void CheckHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        Diagnostic(std::string("SETUP_UI_FAILED ") + what + " hr=" + std::to_string(hr));
        throw winrt::hresult_error(hr);
    }
}

} // namespace

void ShowSetupScreen(const winrt::Windows::UI::Core::CoreWindow& window,
                     const std::string& status_line) {
    try {
        Diagnostic("SETUP_UI_BEGIN");
        ComPtr<ID3D11Device> d3d_device;
        ComPtr<ID3D11DeviceContext> d3d_context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        CheckHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                  D3D11_SDK_VERSION, &d3d_device, nullptr, &d3d_context),
               "D3D11CreateDevice");

        ComPtr<IDXGIDevice> dxgi_device;
        CheckHr(d3d_device.As(&dxgi_device), "QI IDXGIDevice");

        ComPtr<ID2D1Factory1> d2d_factory;
        CheckHr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                  __uuidof(ID2D1Factory1),
                                  reinterpret_cast<void**>(d2d_factory.GetAddressOf())),
               "D2D1CreateFactory");
        ComPtr<ID2D1Device> d2d_device;
        CheckHr(d2d_factory->CreateDevice(dxgi_device.Get(), &d2d_device), "CreateDevice");
        ComPtr<ID2D1DeviceContext> d2d_context;
        CheckHr(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context),
               "CreateDeviceContext");

        ComPtr<IDXGIAdapter> dxgi_adapter;
        CheckHr(dxgi_device->GetAdapter(&dxgi_adapter), "GetAdapter");
        ComPtr<IDXGIFactory2> dxgi_factory;
        CheckHr(dxgi_adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)), "GetParent factory");

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 1920;
        desc.Height = 1080;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo = false;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        // The Xbox compositor is a fixed-refresh sink, same reasoning as the Mesa present patch
        // (patch_mesa_uwp.py): no tearing/discard flags here.

        ComPtr<IDXGISwapChain1> swap_chain;
        CheckHr(dxgi_factory->CreateSwapChainForCoreWindow(
                   d3d_device.Get(), winrt::get_unknown(window), &desc, nullptr, &swap_chain),
               "CreateSwapChainForCoreWindow");

        ComPtr<IDXGISurface> back_buffer;
        CheckHr(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "GetBuffer");
        const auto bitmap_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        ComPtr<ID2D1Bitmap1> target;
        CheckHr(d2d_context->CreateBitmapFromDxgiSurface(back_buffer.Get(), &bitmap_properties,
                                                         &target),
               "CreateBitmapFromDxgiSurface");
        d2d_context->SetTarget(target.Get());

        ComPtr<IDWriteFactory> dwrite_factory;
        CheckHr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(dwrite_factory.GetAddressOf())),
               "DWriteCreateFactory");
        ComPtr<IDWriteTextFormat> title_format;
        CheckHr(dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, 64.0f, L"en-us",
                                                 &title_format),
               "CreateTextFormat title");
        ComPtr<IDWriteTextFormat> status_format;
        CheckHr(dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us",
                                                 &status_format),
               "CreateTextFormat status");

        ComPtr<ID2D1SolidColorBrush> accent_brush;
        CheckHr(d2d_context->CreateSolidColorBrush(kAccent, &accent_brush), "brush accent");
        ComPtr<ID2D1SolidColorBrush> text_brush;
        CheckHr(d2d_context->CreateSolidColorBrush(kText, &text_brush), "brush text");

        const std::wstring status_wide(status_line.begin(), status_line.end());

        // A few seconds is enough to prove the whole D3D11 + D2D + DirectWrite + CoreWindow
        // pipeline renders on real Xbox hardware; a real setup screen (input, navigation between
        // steps) is the next increment, not this one.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < until) {
            window.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            d2d_context->BeginDraw();
            d2d_context->Clear(kBackground);
            d2d_context->DrawText(L"NXbox", 5, title_format.Get(),
                                  D2D1::RectF(56.0f, 56.0f, 1000.0f, 160.0f), accent_brush.Get());
            d2d_context->DrawText(status_wide.c_str(), static_cast<UINT32>(status_wide.size()),
                                  status_format.Get(), D2D1::RectF(56.0f, 180.0f, 1864.0f, 260.0f),
                                  text_brush.Get());
            CheckHr(d2d_context->EndDraw(), "EndDraw");
            const DXGI_PRESENT_PARAMETERS present_params{};
            CheckHr(swap_chain->Present1(1, 0, &present_params), "Present1");
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Diagnostic("SETUP_UI_END");
    } catch (const winrt::hresult_error& error) {
        Diagnostic("SETUP_UI_FAILED " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("SETUP_UI_FAILED ") + error.what());
    }
}

} // namespace EdenXbox
