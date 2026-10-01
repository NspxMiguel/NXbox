// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/renderer.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>

#include <fmt/format.h>

#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/theme.h"

namespace EdenXbox::Ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kFontCount = static_cast<std::size_t>(Font::Count);

struct FontSpec {
    bool mono;
    DWRITE_FONT_WEIGHT weight;
    float size; // canvas units
    bool wrap;  // false: one line, trimmed with an ellipsis
};

// The weight between semi-bold and bold that the approved look asks for on pills and section
// titles; DirectWrite picks the nearest face the font has.
constexpr DWRITE_FONT_WEIGHT kWeight650 = static_cast<DWRITE_FONT_WEIGHT>(650);

// One row per Font value, in the same order as the enum. Sizes are the approved look's, in canvas
// units.
constexpr FontSpec kFontSpecs[] = {
    {false, DWRITE_FONT_WEIGHT_SEMI_BOLD, 21.0f, false}, // Nav
    {true, DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f, false},  // Clock
    {false, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 72.0f, true}, // Title
    {false, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 52.0f, true}, // TitleSmall
    {false, DWRITE_FONT_WEIGHT_REGULAR, 22.0f, false},   // Meta
    {true, DWRITE_FONT_WEIGHT_REGULAR, 21.0f, false},    // MetaMono
    {true, DWRITE_FONT_WEIGHT_SEMI_BOLD, 21.0f, false},  // MetaStrong
    {false, kWeight650, 24.0f, false},                   // Button
    {false, kWeight650, 22.0f, false},                   // Section
    {false, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f, false}, // Caption
    {false, DWRITE_FONT_WEIGHT_SEMI_BOLD, 18.0f, true},  // TileLabel
    {false, DWRITE_FONT_WEIGHT_REGULAR, 19.0f, false},   // Hint
    {false, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 15.0f, false}, // Glyph
    {false, DWRITE_FONT_WEIGHT_BOLD, 40.0f, true},       // Heading
    {false, DWRITE_FONT_WEIGHT_REGULAR, 24.0f, true},    // Body
    {false, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 64.0f, false}, // ModsTitle
    {false, kWeight650, 25.0f, false},                   // RowTitle
    {false, DWRITE_FONT_WEIGHT_REGULAR, 19.0f, false},   // RowSub
    {true, DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f, false},  // Stat
    {false, DWRITE_FONT_WEIGHT_MEDIUM, 15.0f, false},    // StatCaption
    {true, DWRITE_FONT_WEIGHT_REGULAR, 16.0f, false},    // ChipCount
    {true, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 112.0f, false}, // Code
};
static_assert(std::size(kFontSpecs) == kFontCount, "every Font needs a row in kFontSpecs");

void CheckHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        Diagnostic(
            fmt::format("UI renderer failed {} hr={:#010x}", what, static_cast<unsigned>(hr)));
        throw winrt::hresult_error(hr);
    }
}

// For per-image failures, which must not take the whole screen down.
bool Succeeded(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) {
        return true;
    }
    Diagnostic(fmt::format("UI renderer {} failed hr={:#010x}", what, static_cast<unsigned>(hr)));
    return false;
}

DWRITE_TEXT_ALIGNMENT ToDWrite(HAlign align) {
    switch (align) {
    case HAlign::Center:
        return DWRITE_TEXT_ALIGNMENT_CENTER;
    case HAlign::Right:
        return DWRITE_TEXT_ALIGNMENT_TRAILING;
    case HAlign::Left:
    default:
        return DWRITE_TEXT_ALIGNMENT_LEADING;
    }
}

DWRITE_PARAGRAPH_ALIGNMENT ToDWrite(VAlign align) {
    switch (align) {
    case VAlign::Middle:
        return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    case VAlign::Bottom:
        return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
    case VAlign::Top:
    default:
        return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    }
}

} // namespace

Renderer::~Renderer() {
    Shutdown();
}

void Renderer::Initialize(const winrt::Windows::UI::Core::CoreWindow& window) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    CheckHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                              D3D11_SDK_VERSION, &d3d_device_, nullptr, &d3d_context_),
            "D3D11CreateDevice");
    CheckHr(d3d_device_.As(&dxgi_device_), "QI IDXGIDevice");

    CheckHr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                              reinterpret_cast<void**>(d2d_factory_.GetAddressOf())),
            "D2D1CreateFactory");
    CheckHr(d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_), "CreateDevice");
    CheckHr(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_),
            "CreateDeviceContext");

    ComPtr<IDXGIAdapter> adapter;
    CheckHr(dxgi_device_->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> dxgi_factory;
    CheckHr(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)), "GetParent factory");

    // The same flip swap chain setup_ui.cpp proved on the console: a fixed 1920x1080 surface that
    // the compositor stretches to the display, and no tearing or discard flags.
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
    CheckHr(dxgi_factory->CreateSwapChainForCoreWindow(d3d_device_.Get(),
                                                       winrt::get_unknown(window), &desc, nullptr,
                                                       &swap_chain_),
            "CreateSwapChainForCoreWindow");

    ComPtr<IDXGISurface> back_buffer;
    CheckHr(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "GetBuffer");
    const auto target_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    CheckHr(context_->CreateBitmapFromDxgiSurface(back_buffer.Get(), &target_properties, &target_),
            "CreateBitmapFromDxgiSurface");
    context_->SetTarget(target_.Get());
    context_->SetDpi(96.0f, 96.0f);
    // Grayscale text: ClearType assumes an LCD panel and an opaque background, and the UI draws
    // over gradients and rotated tiles.
    context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Canvas units to pixels: uniform scale, centered when the surface is not 16:9.
    const float width = static_cast<float>(desc.Width);
    const float height = static_cast<float>(desc.Height);
    const float scale = std::min(width / kCanvasWidth, height / kCanvasHeight);
    canvas_ = D2D1::Matrix3x2F::Scale(scale, scale) *
              D2D1::Matrix3x2F::Translation((width - kCanvasWidth * scale) * 0.5f,
                                            (height - kCanvasHeight * scale) * 0.5f);

    const D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
    CheckHr(context_->CreateSolidColorBrush(white, &solid_), "CreateSolidColorBrush");

    CheckHr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())),
            "DWriteCreateFactory");
    CreateTextFormats();
    CheckHr(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&wic_)),
            "CoCreateInstance WIC");
    Diagnostic(fmt::format("UI renderer ready {}x{} scale={:.2f}", desc.Width, desc.Height, scale));
}

void Renderer::CreateTextFormats() {
    // The first monospaced face the console has; Segoe UI digits are tabular as a last resort.
    const wchar_t* mono_family = L"Segoe UI";
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts, FALSE))) {
        for (const wchar_t* family : {L"Consolas", L"Cascadia Mono", L"Courier New"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) {
                mono_family = family;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < kFontCount; ++i) {
        const FontSpec& spec = kFontSpecs[i];
        CheckHr(dwrite_->CreateTextFormat(spec.mono ? mono_family : L"Segoe UI", nullptr,
                                          spec.weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, spec.size, L"en-us",
                                          &formats_[i]),
                "CreateTextFormat");
        if (spec.wrap) {
            CheckHr(formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP), "SetWordWrapping");
            continue;
        }
        CheckHr(formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "SetWordWrapping");
        ComPtr<IDWriteInlineObject> ellipsis;
        CheckHr(dwrite_->CreateEllipsisTrimmingSign(formats_[i].Get(), &ellipsis),
                "CreateEllipsisTrimmingSign");
        const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        CheckHr(formats_[i]->SetTrimming(&trimming, ellipsis.Get()), "SetTrimming");
    }
}

void Renderer::Shutdown() {
    const bool was_open = static_cast<bool>(swap_chain_);
    if (drawing_ && context_) {
        // A draw call threw before EndFrame(): close the frame so the target can be released.
        void(context_->EndDraw());
        drawing_ = false;
    }
    if (context_) {
        context_->SetTarget(nullptr);
    }
    // Everything that references the swap chain's back buffer or the device goes first, in the
    // reverse order of creation; the swap chain is the last DXGI object to be released.
    for (auto& format : formats_) {
        format.Reset();
    }
    wic_.Reset();
    dwrite_.Reset();
    solid_.Reset();
    target_.Reset();
    context_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    swap_chain_.Reset();
    if (d3d_context_) {
        // The D3D11 runtime releases objects lazily: unbind everything, flush, and wait until the
        // GPU is done, so the window has no swap chain left when Mesa creates its own.
        d3d_context_->ClearState();
        d3d_context_->Flush();
        WaitForGpu();
    }
    d3d_context_.Reset();
    dxgi_device_.Reset();
    d3d_device_.Reset();
    if (was_open) {
        Diagnostic("UI renderer released");
    }
}

void Renderer::WaitForGpu() {
    D3D11_QUERY_DESC description{};
    description.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> query;
    if (!d3d_device_ || FAILED(d3d_device_->CreateQuery(&description, &query))) {
        return;
    }
    d3d_context_->End(query.Get());
    d3d_context_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (d3d_context_->GetData(query.Get(), nullptr, 0, 0) == S_FALSE &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Renderer::Trim() {
    ComPtr<IDXGIDevice3> device3;
    if (dxgi_device_ && SUCCEEDED(dxgi_device_.As(&device3))) {
        device3->Trim();
    }
}

void Renderer::BeginFrame() {
    context_->BeginDraw();
    drawing_ = true;
    context_->SetTransform(D2D1::Matrix3x2F::Identity());
    context_->Clear(Theme::kBackground);
    context_->SetTransform(canvas_);
}

void Renderer::EndFrame() {
    context_->SetTransform(D2D1::Matrix3x2F::Identity());
    drawing_ = false;
    CheckHr(context_->EndDraw(), "EndDraw");
    const DXGI_PRESENT_PARAMETERS parameters{};
    CheckHr(swap_chain_->Present1(1, 0, &parameters), "Present1");
}

void Renderer::SetLocalTransform(const D2D1::Matrix3x2F& local) {
    context_->SetTransform(local * canvas_);
}

void Renderer::ClearLocalTransform() {
    context_->SetTransform(canvas_);
}

void Renderer::PushClip(const D2D1_RECT_F& rect) {
    context_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderer::PopClip() {
    context_->PopAxisAlignedClip();
}

void Renderer::FillRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color) {
    solid_->SetColor(color);
    if (radius <= 0.0f) {
        context_->FillRectangle(rect, solid_.Get());
    } else {
        context_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), solid_.Get());
    }
}

void Renderer::StrokeRounded(const D2D1_RECT_F& rect, float radius, float width,
                             const D2D1_COLOR_F& color) {
    solid_->SetColor(color);
    context_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), solid_.Get(), width);
}

void Renderer::FillDisc(D2D1_POINT_2F center, float radius, const D2D1_COLOR_F& color) {
    solid_->SetColor(color);
    context_->FillEllipse(D2D1::Ellipse(center, radius, radius), solid_.Get());
}

void Renderer::FillPolygon(const D2D1_POINT_2F* points, UINT32 count, const D2D1_COLOR_F& color) {
    if (count < 3) {
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    CheckHr(d2d_factory_->CreatePathGeometry(&geometry), "CreatePathGeometry");
    ComPtr<ID2D1GeometrySink> sink;
    CheckHr(geometry->Open(&sink), "PathGeometry::Open");
    sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points + 1, count - 1);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    CheckHr(sink->Close(), "GeometrySink::Close");
    solid_->SetColor(color);
    context_->FillGeometry(geometry.Get(), solid_.Get());
}

ComPtr<ID2D1LinearGradientBrush> Renderer::MakeGradient(const D2D1_GRADIENT_STOP* stops,
                                                        UINT32 count, D2D1_POINT_2F from,
                                                        D2D1_POINT_2F to, float opacity) {
    // The plain render target interface: ID2D1DeviceContext adds overloads of the same names.
    ID2D1RenderTarget* render_target = context_.Get();
    ComPtr<ID2D1GradientStopCollection> collection;
    CheckHr(render_target->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                        D2D1_EXTEND_MODE_CLAMP, &collection),
            "CreateGradientStopCollection");
    const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES properties = {from, to};
    ComPtr<ID2D1LinearGradientBrush> brush;
    CheckHr(render_target->CreateLinearGradientBrush(&properties, nullptr, collection.Get(),
                                                     &brush),
            "CreateLinearGradientBrush");
    brush->SetOpacity(opacity);
    return brush;
}

void Renderer::FillGradient(const D2D1_RECT_F& rect, float radius, const D2D1_GRADIENT_STOP* stops,
                            UINT32 count, D2D1_POINT_2F from, D2D1_POINT_2F to, float opacity) {
    const ComPtr<ID2D1LinearGradientBrush> brush = MakeGradient(stops, count, from, to, opacity);
    if (radius <= 0.0f) {
        context_->FillRectangle(rect, brush.Get());
    } else {
        context_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    }
}

void Renderer::StrokeGradient(const D2D1_RECT_F& rect, float radius, float width,
                              const D2D1_GRADIENT_STOP* stops, UINT32 count, D2D1_POINT_2F from,
                              D2D1_POINT_2F to, float opacity) {
    const ComPtr<ID2D1LinearGradientBrush> brush = MakeGradient(stops, count, from, to, opacity);
    context_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
}

bool Renderer::DecodeImage(const std::vector<std::uint8_t>& encoded, Pixels& pixels) {
    if (encoded.empty() || !wic_) {
        return false;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0;
    UINT height = 0;
    if (!Succeeded(wic_->CreateStream(&stream), "WIC CreateStream") ||
        !Succeeded(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                                static_cast<DWORD>(encoded.size())),
                   "WIC InitializeFromMemory") ||
        !Succeeded(wic_->CreateDecoderFromStream(stream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnDemand, &decoder),
                   "WIC CreateDecoderFromStream") ||
        !Succeeded(decoder->GetFrame(0, &frame), "WIC GetFrame") ||
        !Succeeded(wic_->CreateFormatConverter(&converter), "WIC CreateFormatConverter") ||
        !Succeeded(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut),
                   "WIC converter Initialize") ||
        !Succeeded(converter->GetSize(&width, &height), "WIC GetSize")) {
        return false;
    }
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        Diagnostic(fmt::format("UI renderer image size {}x{} rejected", width, height));
        return false;
    }
    pixels.width = width;
    pixels.height = height;
    pixels.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    return Succeeded(converter->CopyPixels(nullptr, width * 4,
                                           static_cast<UINT>(pixels.bgra.size()),
                                           pixels.bgra.data()),
                     "WIC CopyPixels");
}

bool Renderer::UploadImage(const Pixels& pixels, Image& image) {
    const auto bitmap_properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> bitmap;
    if (!Succeeded(context_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height),
                                          pixels.bgra.data(), pixels.width * 4,
                                          &bitmap_properties, &bitmap),
                   "CreateBitmap")) {
        return false;
    }
    D2D1_BITMAP_BRUSH_PROPERTIES1 brush_properties{};
    brush_properties.extendModeX = D2D1_EXTEND_MODE_CLAMP;
    brush_properties.extendModeY = D2D1_EXTEND_MODE_CLAMP;
    brush_properties.interpolationMode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    ComPtr<ID2D1BitmapBrush1> brush;
    if (!Succeeded(context_->CreateBitmapBrush(bitmap.Get(), &brush_properties, nullptr, &brush),
                   "CreateBitmapBrush")) {
        return false;
    }
    image.bitmap = bitmap;
    image.brush = brush;
    image.size = D2D1::SizeF(static_cast<float>(pixels.width), static_cast<float>(pixels.height));
    return true;
}

void Renderer::DrawImage(const Image& image, const D2D1_RECT_F& rect, float radius,
                         float opacity) {
    if (!image.brush) {
        return;
    }
    // The brush space is the space of the shape it fills, so the bitmap follows any local
    // transform (the lifted tile's tilt) together with the rounded rectangle.
    image.brush->SetTransform(
        D2D1::Matrix3x2F::Scale((rect.right - rect.left) / image.size.width,
                                (rect.bottom - rect.top) / image.size.height) *
        D2D1::Matrix3x2F::Translation(rect.left, rect.top));
    image.brush->SetOpacity(opacity);
    context_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), image.brush.Get());
}

ComPtr<IDWriteTextLayout> Renderer::MakeLayout(const std::wstring& text, Font font, float width,
                                               float height) const {
    ComPtr<IDWriteTextLayout> layout;
    CheckHr(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                      formats_[static_cast<std::size_t>(font)].Get(), width,
                                      height, &layout),
            "CreateTextLayout");
    return layout;
}

void Renderer::DrawString(const std::wstring& text, Font font, const D2D1_RECT_F& box,
                          const D2D1_COLOR_F& color, HAlign horizontal, VAlign vertical) {
    if (text.empty()) {
        return;
    }
    const ComPtr<IDWriteTextLayout> layout =
        MakeLayout(text, font, box.right - box.left, box.bottom - box.top);
    CheckHr(layout->SetTextAlignment(ToDWrite(horizontal)), "SetTextAlignment");
    CheckHr(layout->SetParagraphAlignment(ToDWrite(vertical)), "SetParagraphAlignment");
    solid_->SetColor(color);
    context_->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), solid_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
}

TextSize Renderer::MeasureString(const std::wstring& text, Font font, float max_width) const {
    TextSize size;
    if (text.empty()) {
        return size;
    }
    const ComPtr<IDWriteTextLayout> layout = MakeLayout(text, font, max_width, kCanvasHeight);
    DWRITE_TEXT_METRICS metrics{};
    CheckHr(layout->GetMetrics(&metrics), "GetMetrics");
    // Including trailing whitespace, so a run of text and its separator can be laid out in a row.
    size.width = metrics.widthIncludingTrailingWhitespace;
    size.height = metrics.height;
    size.lines = metrics.lineCount;
    return size;
}

} // namespace EdenXbox::Ui
