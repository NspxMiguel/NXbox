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
#include <thread>
#include <glad/glad.h>
#include "common/nxbox_gl_readback.h"

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
        // Every 120th presentation, sample the back buffer so a black screen can be told apart from
        // frames that are presented but empty.
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
        if (++swaps % 120 == 1) {
            NxboxPackBufferGuard pack_guard;
            GLint previous = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            unsigned lit = 0;
            unsigned samples = 0;
            unsigned peak = 0;
            for (int y = 0; y < 36; ++y) {
                for (int x = 0; x < 64; ++x) {
                    unsigned char pixel[4]{};
                    glReadPixels(15 + x * 30, 15 + y * 30, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                 pixel);
                    peak = std::max<unsigned>(peak, std::max({pixel[0], pixel[1], pixel[2]}));
                    ++samples;
                    lit += (pixel[0] | pixel[1] | pixel[2]) != 0;
                }
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
            Diagnostic("BACKBUFFER swap=" + std::to_string(swaps) + " lit=" + std::to_string(lit) +
                       "/" + std::to_string(samples) + " peak=" + std::to_string(peak) +
                       " error=" + std::to_string(glGetError()));
        }
        const auto swap = runtime->Function<BOOL(WINAPI*)(HDC)>("wglSwapBuffers");
        if (!swap(runtime->dc)) {
            throw std::runtime_error("Mesa presentation failed");
        }
    }

private:
    std::shared_ptr<MesaRuntime> runtime;
    HANDLE context;
    unsigned swaps = 0;
    GLuint vertex_array = 0;
};

namespace {
// Renders into an offscreen texture with the plain GL calls Eden relies on and reports what reads
// back, so a broken driver path can be told apart from a broken emulator path.
void RunRenderSelfTest(const char* where) {
    Diagnostic(std::string("SELFTEST begin ") + where);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    Diagnostic("SELFTEST fbo status=" + std::to_string(glCheckFramebufferStatus(GL_FRAMEBUFFER)));
    glViewport(0, 0, 256, 256);
    glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4]{};
    glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    Diagnostic("SELFTEST clear readback=" + std::to_string(pixel[0]) + "," +
               std::to_string(pixel[1]) + "," + std::to_string(pixel[2]) + "," +
               std::to_string(pixel[3]) + " (expect 255,127,63,255)");
    // Same clear on the render target formats the emulator's games commonly use.
    for (const auto& [name, internal_format] :
         std::array<std::pair<const char*, GLenum>, 5>{{{"RGB10_A2", GL_RGB10_A2},
                                                        {"RGBA16F", GL_RGBA16F},
                                                        {"SRGB8_A8", GL_SRGB8_ALPHA8},
                                                        {"R11G11B10F", GL_R11F_G11F_B10F},
                                                        {"RGBA8", GL_RGBA8}}}) {
        for (const bool srgb : {false, true}) {
            GLuint format_texture = 0;
            glGenTextures(1, &format_texture);
            glBindTexture(GL_TEXTURE_2D, format_texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, internal_format, 256, 256);
            GLuint format_fbo = 0;
            glGenFramebuffers(1, &format_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, format_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   format_texture, 0);
            if (srgb) {
                glEnable(GL_FRAMEBUFFER_SRGB);
            }
            const float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
            glClearBufferfv(GL_COLOR, 0, color);
            unsigned char px[4]{};
            glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            Diagnostic(std::string("SELFTEST format ") + name + (srgb ? " srgb" : "     ") +
                       " status=" + std::to_string(glCheckFramebufferStatus(GL_FRAMEBUFFER)) +
                       " readback=" + std::to_string(px[0]) + "," + std::to_string(px[1]) + "," +
                       std::to_string(px[2]) + "," + std::to_string(px[3]) +
                       " error=" + std::to_string(glGetError()));
            glDisable(GL_FRAMEBUFFER_SRGB);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &format_fbo);
            glDeleteTextures(1, &format_texture);
        }
    }
    // Eden creates textures with direct state access and renders into texture views.
    for (const GLenum internal_format : {GL_RGBA8, GL_RGB10_A2}) {
        for (const int variant : {0, 1, 2}) {
            GLuint base = 0;
            glCreateTextures(GL_TEXTURE_2D, 1, &base);
            glTextureStorage2D(base, 1, internal_format, 256, 256);
            GLuint attach = base;
            GLuint view = 0;
            if (variant >= 1) {
                glGenTextures(1, &view);
                glTextureView(view, GL_TEXTURE_2D, base, internal_format, 0, 1, 0, 1);
                attach = view;
            }
            GLuint dsa_fbo = 0;
            glCreateFramebuffers(1, &dsa_fbo);
            glNamedFramebufferTexture(dsa_fbo, GL_COLOR_ATTACHMENT0, attach, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, dsa_fbo);
            if (variant == 2) {
                const GLenum buffers[] = {GL_COLOR_ATTACHMENT0};
                glNamedFramebufferDrawBuffers(dsa_fbo, 1, buffers);
            }
            const float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
            glClearBufferfv(GL_COLOR, 0, color);
            unsigned char base_px[4]{};
            glGetTextureSubImage(base, 0, 128, 128, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4,
                                 base_px);
            unsigned char fbo_px[4]{};
            glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fbo_px);
            Diagnostic(std::string("SELFTEST dsa ") +
                       (internal_format == GL_RGBA8 ? "RGBA8" : "RGB10_A2") + " variant=" +
                       std::to_string(variant) + " texture=" + std::to_string(base_px[0]) + "," +
                       std::to_string(base_px[1]) + " fbo=" + std::to_string(fbo_px[0]) + "," +
                       std::to_string(fbo_px[1]) + " error=" + std::to_string(glGetError()));
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &dsa_fbo);
            if (view) {
                glDeleteTextures(1, &view);
            }
            glDeleteTextures(1, &base);
        }
    }
    const char* vertex_source =
        "#version 430 core\nvoid main(){vec2 p=vec2((gl_VertexID&1)*2-1,(gl_VertexID>>1)*2-1);"
        "gl_Position=vec4(p,0.0,1.0);}\n";
    const char* fragment_source =
        "#version 430 core\nlayout(location=0) out vec4 c;void main(){c=vec4(0.0,1.0,0.0,1.0);}\n";
    const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_source, nullptr);
    glCompileShader(vs);
    const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment_source, nullptr);
    glCompileShader(fs);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glUseProgram(program);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    Diagnostic("SELFTEST draw linked=" + std::to_string(linked) + " readback=" +
               std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," +
               std::to_string(pixel[2]) + "," + std::to_string(pixel[3]) +
               " (expect 0,255,0,255) error=" + std::to_string(glGetError()));
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
}
} // namespace

MesaWindow::MesaWindow(const CoreWindow& window_, u32 width, u32 height)
    : window(window_), runtime(std::make_shared<MesaRuntime>()) {
    runtime->Initialize(window);
    loader_library = runtime->library;
    if (!gladLoadGLLoader(ResolveGL)) {
        throw std::runtime_error("Cannot load OpenGL entry points");
    }
    RunRenderSelfTest("bootstrap context");
    runtime->make_current(nullptr, nullptr);
    window_info.type = Core::Frontend::WindowSystemType::Windows;
    window_info.render_surface = get_abi(window);
    window_info.render_surface_scale = 1.0f;
    UpdateCurrentFramebufferLayout(width, height);
}

MesaWindow::~MesaWindow() = default;

void MesaWindow::RunSharedSelfTest() {
    // The emulator renders on a shared context owned by its GPU thread; repeat the test there.
    auto shared = CreateSharedContext();
    shared->MakeCurrent();
    RunRenderSelfTest("shared worker context");
    shared->DoneCurrent();
}

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
