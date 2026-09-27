// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/mesa_window.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <winrt/Windows.Storage.h>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
#include <glad/glad.h>

using namespace winrt;
using namespace Windows::UI::Core;

namespace EdenXbox {
namespace {
HMODULE loader_library = nullptr;
void* ResolveGL(const char* name) {
    auto address = GetProcAddress(loader_library, name);
    if (!address) {
        const auto resolve = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(
            GetProcAddress(loader_library, "wglGetProcAddress"));
        if (resolve)
            address = resolve(name);
    }
    return reinterpret_cast<void*>(address);
}
} // namespace
struct PixelFormat {
    WORD size = sizeof(PixelFormat);
    WORD version = 1;
    DWORD flags = 0x00000004 | 0x00000020 | 0x00000001;
    BYTE type = 0;
    BYTE color_bits = 32;
    BYTE red_bits = 0, red_shift = 0, green_bits = 0, green_shift = 0;
    BYTE blue_bits = 0, blue_shift = 0, alpha_bits = 8, alpha_shift = 0;
    BYTE accum_bits = 0, accum_red_bits = 0, accum_green_bits = 0;
    BYTE accum_blue_bits = 0, accum_alpha_bits = 0;
    BYTE depth_bits = 24, stencil_bits = 8, auxiliary_buffers = 0;
    BYTE layer_type = 0, reserved = 0;
    DWORD layer_mask = 0, visible_mask = 0, damage_mask = 0;
};
static_assert(sizeof(PixelFormat) == 40);

struct MesaRuntime {
    HMODULE library = nullptr;
    using ResolveProc = PROC(WINAPI*)(LPCSTR);
    ResolveProc resolve = nullptr;
    HDC dc = nullptr;
    HANDLE context = nullptr;
    using MakeCurrent = BOOL(WINAPI*)(HDC, HANDLE);
    using DeleteContext = BOOL(WINAPI*)(HANDLE);
    MakeCurrent make_current = nullptr;
    DeleteContext delete_context = nullptr;

    template <typename T>
    T Function(const char* name) {
        PROC address = GetProcAddress(library, name);
        if (!address && resolve) {
            address = resolve(name);
        }
        if (!address || address == reinterpret_cast<PROC>(1) ||
            address == reinterpret_cast<PROC>(2) || address == reinterpret_cast<PROC>(3) ||
            address == reinterpret_cast<PROC>(-1)) {
            throw std::runtime_error(std::string("Missing GL entry point: ") + name);
        }
        return reinterpret_cast<T>(address);
    }

    ~MesaRuntime() {
        if (make_current) {
            make_current(nullptr, nullptr);
        }
        if (context && delete_context) {
            delete_context(context);
        }
        if (library) {
            FreeLibrary(library);
        }
    }

    void Initialize(const CoreWindow& window) {
        _putenv_s("GALLIUM_DRIVER", "d3d12");
        Diagnostic("loading packaged DXIL validator");
        const auto validator = LoadPackagedLibrary(L"dxil.dll", 0);
        if (!validator) {
            throw std::runtime_error("Cannot load DXIL validator: " +
                                     std::to_string(GetLastError()));
        }
        // Keep the validator loaded for the process lifetime; Mesa also acquires
        // it.
        Diagnostic("DXIL validator loaded");
        Diagnostic("loading packaged Mesa");
        library = LoadPackagedLibrary(L"opengl32.dll", 0);
        if (!library) {
            throw std::runtime_error("LoadPackagedLibrary failed: " +
                                     std::to_string(GetLastError()));
        }
        resolve = Function<ResolveProc>("wglGetProcAddress");
        const auto choose = Function<int(WINAPI*)(HDC, const PixelFormat*)>("wglChoosePixelFormat");
        const auto set = Function<BOOL(WINAPI*)(HDC, int, const PixelFormat*)>("wglSetPixelFormat");
        const auto create = Function<HANDLE(WINAPI*)(HDC)>("wglCreateContext");
        make_current = Function<MakeCurrent>("wglMakeCurrent");
        delete_context = Function<DeleteContext>("wglDeleteContext");
        // Mesa's UWP GDI shim uses the CoreWindow ABI pointer as its device
        // context.
        dc = reinterpret_cast<HDC>(get_abi(window));
        const PixelFormat descriptor{};
        const int format = choose(dc, &descriptor);
        if (!format || !set(dc, format, &descriptor)) {
            throw std::runtime_error("Cannot select a Mesa pixel format");
        }
        Diagnostic("creating bootstrap context");
        context = create(dc);
        if (!context || !make_current(dc, context)) {
            throw std::runtime_error("Cannot activate the Mesa bootstrap context");
        }
        const auto create_profile =
            Function<HANDLE(WINAPI*)(HDC, HANDLE, const int*)>("wglCreateContextAttribsARB");
        constexpr int attributes[] = {0x2091, 4, 0x2092, 6, 0x9126, 1, 0};
        HANDLE modern = create_profile(dc, nullptr, attributes);
        if (!modern) {
            throw std::runtime_error("Driver rejected an OpenGL 4.6 core context");
        }
        make_current(nullptr, nullptr);
        delete_context(context);
        context = modern;
        if (!make_current(dc, context)) {
            throw std::runtime_error("Cannot activate the OpenGL 4.6 context");
        }
        const auto get_string = Function<const unsigned char*(WINAPI*)(unsigned)>("glGetString");
        for (const auto& [name, id] :
             std::array<std::pair<const char*, unsigned>, 4>{{{"vendor", 0x1F00},
                                                              {"renderer", 0x1F01},
                                                              {"version", 0x1F02},
                                                              {"glsl", 0x8B8C}}}) {
            const auto value = get_string(id);
            Diagnostic(std::string(name) + "=" +
                       (value ? reinterpret_cast<const char*>(value) : "null"));
        }
    }
};

class MesaGraphicsContext final : public Core::Frontend::GraphicsContext {
public:
    MesaGraphicsContext(std::shared_ptr<MesaRuntime> runtime_, HANDLE context_)
        : runtime(std::move(runtime_)), context(context_) {}
    ~MesaGraphicsContext() override {
        runtime->delete_context(context);
    }
    void MakeCurrent() override {
        if (!runtime->make_current(runtime->dc, context)) {
            throw std::runtime_error("Mesa context activation failed");
        }
        // Eden's renderer draws with vertex buffers and no vertex array object, which only a
        // compatibility context allows. This is a core profile context, where that draw fails with
        // "No array object bound" and every frame stays black, so give each context one.
        if (vertex_array == 0) {
            glGenVertexArrays(1, &vertex_array);
        }
        glBindVertexArray(vertex_array);
    }
    void DoneCurrent() override {
        runtime->make_current(nullptr, nullptr);
    }
    void SwapBuffers() override {
        // LocalState\present_test.txt replaces every frame with solid red, to tell a broken
        // presentation path from a game that renders nothing.
        static const bool present_test = std::filesystem::exists(
            std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "present_test.txt");
        if (present_test) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        // The Xbox compositor treats the swap chain as premultiplied alpha, so a frame whose alpha
        // channel was left at 0 shows as transparent (black). Force every presented pixel opaque.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const auto swap = runtime->Function<BOOL(WINAPI*)(HDC)>("wglSwapBuffers");
        if (!swap(runtime->dc)) {
            throw std::runtime_error("Mesa presentation failed");
        }
    }

private:
    std::shared_ptr<MesaRuntime> runtime;
    HANDLE context;
    GLuint vertex_array = 0;
};

MesaWindow::MesaWindow(const CoreWindow& window_, u32 width, u32 height)
    : window(window_), runtime(std::make_shared<MesaRuntime>()) {
    runtime->Initialize(window);
    loader_library = runtime->library;
    if (!gladLoadGLLoader(ResolveGL)) {
        throw std::runtime_error("Cannot load OpenGL entry points");
    }
    runtime->make_current(nullptr, nullptr);
    window_info.type = Core::Frontend::WindowSystemType::Windows;
    window_info.render_surface = get_abi(window);
    window_info.render_surface_scale = 1.0f;
    UpdateCurrentFramebufferLayout(width, height);
}

MesaWindow::~MesaWindow() = default;

std::unique_ptr<Core::Frontend::GraphicsContext> MesaWindow::CreateSharedContext() const {
    auto create = [this] {
        const auto function = runtime->Function<HANDLE(WINAPI*)(HDC, HANDLE, const int*)>(
            "wglCreateContextAttribsARB");
        constexpr int attributes[] = {0x2091, 4, 0x2092, 6, 0x9126, 1, 0};
        const auto context = function(runtime->dc, runtime->context, attributes);
        if (!context)
            throw std::runtime_error("Mesa shared context creation failed");
        return context;
    };
    HANDLE context = nullptr;
    const auto dispatcher = window.Dispatcher();
    if (dispatcher.HasThreadAccess()) {
        context = create();
    } else {
        std::promise<HANDLE> promise;
        auto result = promise.get_future();
        dispatcher.RunAsync(CoreDispatcherPriority::Normal, [&] {
            try {
                promise.set_value(create());
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });
        context = result.get();
    }
    return std::make_unique<MesaGraphicsContext>(runtime, context);
}

bool MesaWindow::IsShown() const {
    return true;
}
void MesaWindow::OnFrameDisplayed() {
    frames.fetch_add(1, std::memory_order_relaxed);
}
u64 MesaWindow::FrameCount() const {
    return frames.load(std::memory_order_relaxed);
}
} // namespace EdenXbox
