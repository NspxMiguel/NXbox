// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/Windows.UI.Core.h>

namespace EdenXbox::Ui {

// The UI is laid out on a fixed 1920x1080 canvas that the renderer scales to the swap chain.
inline constexpr float kCanvasWidth = 1920.0f;
inline constexpr float kCanvasHeight = 1080.0f;

// The text styles of the UI. Segoe UI throughout; numbers use a monospaced face.
enum class Font {
    Nav,        // tab labels, the toast
    Clock,      // monospace digits
    Title,      // hero title, 72 px weight 800, wraps
    TitleSmall, // the same for titles that need more than two lines
    Meta,       // the labels and values of the details sheet
    MetaMono,   // the hero meta line
    MetaStrong, // the emphasized numbers of the meta line
    Button,     // pill labels
    Section,    // the title above the rail
    Caption,    // the name under the focused tile
    TileLabel,  // the text inside the "add games" tile, wraps
    Hint,       // hint bar labels
    Glyph,      // the letter inside a controller button glyph
    Heading,    // sheet titles, wraps
    Body,       // paragraphs, wraps
    Count,
};

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

struct TextSize {
    float width = 0.0f;
    float height = 0.0f;
    std::uint32_t lines = 0;
};

// A decoded image as 32-bit premultiplied BGRA.
struct Pixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra;
};

// A decoded image on the GPU. Release it before the renderer goes away.
struct Image {
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> brush;
    D2D1_SIZE_F size{};
};

// Direct2D, DirectWrite and WIC on a D3D11 flip swap chain that belongs to the CoreWindow. The UI
// owns the window only until a game boots: Shutdown() (also run by the destructor) releases every
// D3D11/D2D/DXGI object, the swap chain included, so Mesa can take the window afterwards.
//
// Everything is drawn in canvas units between BeginFrame() and EndFrame(). Calls must come from
// the thread that created the renderer. Failures throw winrt::hresult_error.
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Initialize(const winrt::Windows::UI::Core::CoreWindow& window);
    void Shutdown();
    // Frees memory the driver can spare; the system asks for it when the app is suspended.
    void Trim();

    void BeginFrame();
    void EndFrame(); // presents with vsync

    // A transform applied inside the canvas transform, for the lifted tile.
    void SetLocalTransform(const D2D1::Matrix3x2F& local);
    void ClearLocalTransform();

    // Everything drawn between PushClip and PopClip stays inside `rect` (canvas units, hard edge).
    void PushClip(const D2D1_RECT_F& rect);
    void PopClip();

    void FillRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color);
    // `rect` is the center line of the stroke.
    void StrokeRounded(const D2D1_RECT_F& rect, float radius, float width,
                       const D2D1_COLOR_F& color);
    void FillDisc(D2D1_POINT_2F center, float radius, const D2D1_COLOR_F& color);
    // A convex polygon, for the play glyph.
    void FillPolygon(const D2D1_POINT_2F* points, UINT32 count, const D2D1_COLOR_F& color);
    // Linear gradients run from `from` to `to`, in canvas units.
    void FillGradient(const D2D1_RECT_F& rect, float radius, const D2D1_GRADIENT_STOP* stops,
                      UINT32 count, D2D1_POINT_2F from, D2D1_POINT_2F to, float opacity = 1.0f);
    void StrokeGradient(const D2D1_RECT_F& rect, float radius, float width,
                        const D2D1_GRADIENT_STOP* stops, UINT32 count, D2D1_POINT_2F from,
                        D2D1_POINT_2F to, float opacity = 1.0f);

    // Decodes a JPEG (or any WIC format) and uploads it. False, with a log line, when it fails.
    bool DecodeImage(const std::vector<std::uint8_t>& encoded, Pixels& pixels);
    bool UploadImage(const Pixels& pixels, Image& image);
    // Draws the image stretched over `rect`, clipped to a rounded rectangle.
    void DrawImage(const Image& image, const D2D1_RECT_F& rect, float radius, float opacity);

    void DrawString(const std::wstring& text, Font font, const D2D1_RECT_F& box,
                    const D2D1_COLOR_F& color, HAlign horizontal = HAlign::Left,
                    VAlign vertical = VAlign::Top);
    // Wraps at `max_width` when the font wraps.
    TextSize MeasureString(const std::wstring& text, Font font, float max_width = 4096.0f) const;

private:
    void CreateTextFormats();
    Microsoft::WRL::ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text, Font font,
                                                         float width, float height) const;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> MakeGradient(const D2D1_GRADIENT_STOP* stops,
                                                                  UINT32 count,
                                                                  D2D1_POINT_2F from,
                                                                  D2D1_POINT_2F to,
                                                                  float opacity);
    void WaitForGpu();

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IWICImagingFactory2> wic_;
    std::array<Microsoft::WRL::ComPtr<IDWriteTextFormat>, static_cast<std::size_t>(Font::Count)>
        formats_;
    D2D1::Matrix3x2F canvas_ = D2D1::Matrix3x2F::Identity();
    bool drawing_ = false;
};

} // namespace EdenXbox::Ui
