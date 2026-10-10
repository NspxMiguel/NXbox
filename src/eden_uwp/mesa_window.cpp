// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/nxbox_stall.h"
#include "common/scope_exit.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/mesa_window.h"
#include "eden_uwp/ui/renderer.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <winrt/Windows.Storage.h>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
#include <glad/glad.h>

using namespace winrt;
using namespace winrt::Windows::UI::Core;

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
        // Diagnostic knobs for Mesa, changeable on the console without a rebuild.
        {
            const std::filesystem::path env_file =
                std::filesystem::path(winrt::to_string(
                    winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
                "nxbox_env.txt";
            std::ifstream env_in(env_file);
            std::string line;
            while (std::getline(env_in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                const auto eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) {
                    continue;
                }
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
                _putenv_s(key.c_str(), value.c_str());
                Diagnostic("ENV " + key + "=" + value);
            }
            // Mesa's per-call API ring and per-command journal format strings for every D3D12 call
            // and command (about 30% of the render thread): off unless asked for.
            for (const char* name : {"NXBOX_API_RING", "NXBOX_JOURNAL"}) {
                if (std::getenv(name) == nullptr) {
                    _putenv_s(name, "0");
                }
            }
            if (std::getenv("NXBOX_DRAW_BUCKETS") != nullptr) {
                const std::string log_path = (env_file.parent_path() / "bucket.txt").string();
                std::error_code remove_error;
                std::filesystem::remove(log_path, remove_error);
                _putenv_s("NXBOX_DRAW_BUCKET_LOG", log_path.c_str());
            }
            if (const char* dump = std::getenv("NXBOX_FRAME_DUMP"); dump && *dump == '1') {
                const std::string dump_dir = (env_file.parent_path() / "framedump").string();
                _putenv_s("NXBOX_FRAME_DUMP_DIR", dump_dir.c_str());
                Diagnostic("FRAME_DUMP dir=" + dump_dir);
            }
        }
        Diagnostic("loading packaged DXIL validator");
        const auto validator = LoadPackagedLibrary(L"dxil.dll", 0);
        if (!validator) {
            throw std::runtime_error("Cannot load DXIL validator: " +
                                     std::to_string(GetLastError()));
        }
        // Keep the validator loaded for the process lifetime; Mesa also acquires
        // it.
        Diagnostic("DXIL validator loaded");
        // Optional: lets Mesa turn validation failures into readable messages.
        Diagnostic(LoadPackagedLibrary(L"dxcompiler.dll", 0) ? "DXC compiler loaded"
                                                               : "DXC compiler unavailable");
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
        // Device capabilities the patched driver publishes once its screen exists.
        for (const char* name : {"NXBOX_D3D12_SM", "NXBOX_D3D12_RELAXED_CAST"}) {
            char value[32] = "unset";
            GetEnvironmentVariableA(name, value, sizeof(value));
            Diagnostic(std::string(name) + "=" + value);
        }
    }
};

class MesaGraphicsContext final : public Core::Frontend::GraphicsContext {
public:
    MesaGraphicsContext(std::shared_ptr<MesaRuntime> runtime_, HANDLE context_,
                        bool surfaceless_ = false)
        : runtime(std::move(runtime_)), context(context_), surfaceless(surfaceless_) {}
    ~MesaGraphicsContext() override {
        runtime->delete_context(context);
    }
    void MakeCurrent() override {
        // Shared worker contexts (shader compilation) never draw to the window. Binding them to
        // the window DC made Mesa build a second swap chain for the one CoreWindow, which hangs;
        // Mesa's WGL accepts a context current without a drawable.
        if (surfaceless) {
            Diagnostic("CTX make_current surfaceless begin");
        }
        const bool made = runtime->make_current(surfaceless ? nullptr : runtime->dc, context);
        if (surfaceless) {
            Diagnostic(made ? "CTX make_current surfaceless ok" : "CTX make_current surfaceless FAILED");
        }
        Diagnostic("D3D12_CONTEXT tid=" + std::to_string(GetCurrentThreadId()) +
                   " context=" + std::to_string(reinterpret_cast<uintptr_t>(context)) +
                   " role=" + (surfaceless ? "shader-worker" : "render-worker") +
                   " current=" + (made ? "1" : "0"));
        if (!made) {
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
        NxboxStall::Scope stall_scope{NxboxStall::Kind::Present};
        if (!swap(runtime->dc)) {
            throw std::runtime_error("Mesa presentation failed");
        }
    }

private:
    std::shared_ptr<MesaRuntime> runtime;
    HANDLE context;
    bool surfaceless;
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
    // Build shader-cache pipelines on the emulator's own context: shared worker contexts hang P5R
    // on Mesa d3d12 (the same failure as use_asynchronous_shaders).
    strict_context_required = true;
    // Mesa's WGL on the Xbox cannot make a second context current (no surfaceless contexts, and the
    // window DC would need a second swap chain), so precompile on the renderer's own context.
    precompile_on_current_context = true;
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
    const int ordinal = shared_contexts_created.load();
    Diagnostic("CTX create begin #" + std::to_string(ordinal) +
               (dispatcher.HasThreadAccess() ? " ui-thread" : " via-dispatcher"));
    // NXBOX_CTX_DIRECT=1: create worker contexts on the calling thread. Creation through the UI
    // dispatcher hung for the second context (the first, the renderer's, succeeds that way).
    const char* direct = std::getenv("NXBOX_CTX_DIRECT");
    const bool create_here = ordinal > 0 && direct != nullptr && direct[0] == '1';
    if (dispatcher.HasThreadAccess() || create_here) {
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
    Diagnostic("CTX create end #" + std::to_string(ordinal));
    // The first shared context is the renderer's (video_core CreateGPU) and presents to the
    // window; later ones are shader workers and the CPU-side context, which stay surfaceless.
    const bool surfaceless = shared_contexts_created++ > 0;
    return std::make_unique<MesaGraphicsContext>(runtime, context, surfaceless);
}

void MesaWindow::PresentLaunchFrame(const Ui::Pixels& pixels, bool bootstrap) {
    if (bootstrap && !runtime->make_current(runtime->dc, runtime->context)) {
        throw std::runtime_error("Cannot activate launch context");
    }
    SCOPE_EXIT {
        if (bootstrap) {
            runtime->make_current(nullptr, nullptr);
        }
    };
    GLint read = 0, draw = 0, texture = 0, unpack = 0;
    GLint alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    const bool srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLuint image = 0, framebuffer = 0;
    glGenTextures(1, &image);
    glGenFramebuffers(1, &framebuffer);
    SCOPE_EXIT {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
        if (srgb)
            glEnable(GL_FRAMEBUFFER_SRGB);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &image);
    };
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glBindTexture(GL_TEXTURE_2D, image);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pixels.width, pixels.height, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, pixels.bgra.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Launch framebuffer is incomplete");
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // WIC/Direct2D rows start at the top; the GL framebuffer starts at the bottom.
    glBlitFramebuffer(0, 0, pixels.width, pixels.height, 0, 1080, 1920, 0, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    const auto swap = runtime->Function<BOOL(WINAPI*)(HDC)>("wglSwapBuffers");
    if (!swap(runtime->dc)) {
        throw std::runtime_error("Launch presentation failed");
    }
}

bool MesaWindow::IsShown() const {
    return true;
}
void MesaWindow::OnFrameDisplayed() {
    if (frames.fetch_add(1, std::memory_order_relaxed) == 0) {
        Diagnostic("GAME_FIRST_FRAME");
    }
}
u64 MesaWindow::FrameCount() const {
    return frames.load(std::memory_order_relaxed);
}
} // namespace EdenXbox
