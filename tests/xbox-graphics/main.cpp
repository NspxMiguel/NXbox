// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone Mesa/UWP compatibility probe; its presentation rate is not game
// FPS.
#include "common/sparse_memory.h"
#include "eden_shaders.h"
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {
void Log(const std::string &message) noexcept {
  OutputDebugStringA((message + "\n").c_str());
  try {
    const auto path = to_string(
        Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    std::ofstream(path + "\\graphics-probe.txt", std::ios::app)
        << message << '\n';
  } catch (...) {
    OutputDebugStringA("NXbox graphics diagnostic file unavailable\n");
  }
}

void CheckSparseMemory() {
  constexpr std::size_t size = std::size_t{4} << 30;
  const auto before = Windows::System::MemoryManager::AppMemoryUsage();
  void *allocation = Common::SparseMemory::Allocate(size);
  if (!allocation)
    throw std::runtime_error("Sparse memory reservation failed");
  volatile auto *bytes = static_cast<unsigned char *>(allocation);
  const bool zero = bytes[0] == 0 && bytes[size - 1] == 0;
  bytes[0] = 19;
  bytes[size / 2] = 23;
  bytes[size - 1] = 29;
  const bool stored =
      bytes[0] == 19 && bytes[size / 2] == 23 && bytes[size - 1] == 29;
  const auto after = Windows::System::MemoryManager::AppMemoryUsage();
  const bool released = Common::SparseMemory::Free(allocation);
  if (!zero || !stored || !released ||
      (after > before && after - before > 8 * 1024 * 1024)) {
    throw std::runtime_error("Sparse memory validation failed");
  }
  Log("SPARSE_MEMORY_PASS reserved=" + std::to_string(size) + " usage_before=" +
      std::to_string(before) + " usage_after=" + std::to_string(after));
}

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

struct Mesa {
  HMODULE library = nullptr;
  using ResolveProc = PROC(WINAPI *)(LPCSTR);
  ResolveProc resolve = nullptr;
  HDC dc = nullptr;
  HANDLE context = nullptr;
  using MakeCurrent = BOOL(WINAPI *)(HDC, HANDLE);
  using DeleteContext = BOOL(WINAPI *)(HANDLE);
  MakeCurrent make_current = nullptr;
  DeleteContext delete_context = nullptr;

  template <typename T> T Function(const char *name) {
    PROC address = GetProcAddress(library, name);
    if (!address && resolve) {
      address = resolve(name);
    }
    if (!address || address == reinterpret_cast<PROC>(1) ||
        address == reinterpret_cast<PROC>(2) ||
        address == reinterpret_cast<PROC>(3) ||
        address == reinterpret_cast<PROC>(-1)) {
      throw std::runtime_error(std::string("Missing GL entry point: ") + name);
    }
    return reinterpret_cast<T>(address);
  }

  ~Mesa() {
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

  void Initialize(const CoreWindow &window) {
    _putenv_s("GALLIUM_DRIVER", "d3d12");
    Log("loading packaged DXIL validator");
    const auto validator = LoadPackagedLibrary(L"dxil.dll", 0);
    if (!validator) {
      throw std::runtime_error("Cannot load DXIL validator: " +
                               std::to_string(GetLastError()));
    }
    // Keep the validator loaded for the process lifetime; Mesa also acquires
    // it.
    Log("DXIL validator loaded");
    Log("loading packaged Mesa");
    library = LoadPackagedLibrary(L"opengl32.dll", 0);
    if (!library) {
      throw std::runtime_error("LoadPackagedLibrary failed: " +
                               std::to_string(GetLastError()));
    }
    resolve = Function<ResolveProc>("wglGetProcAddress");
    const auto choose = Function<int(WINAPI *)(HDC, const PixelFormat *)>(
        "wglChoosePixelFormat");
    const auto set = Function<BOOL(WINAPI *)(HDC, int, const PixelFormat *)>(
        "wglSetPixelFormat");
    const auto create = Function<HANDLE(WINAPI *)(HDC)>("wglCreateContext");
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
    Log("creating bootstrap context");
    context = create(dc);
    if (!context || !make_current(dc, context)) {
      throw std::runtime_error("Cannot activate the Mesa bootstrap context");
    }
    const auto create_profile =
        Function<HANDLE(WINAPI *)(HDC, HANDLE, const int *)>(
            "wglCreateContextAttribsARB");
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
    const auto get_string =
        Function<const unsigned char *(WINAPI *)(unsigned)>("glGetString");
    for (const auto &[name, id] :
         std::array<std::pair<const char *, unsigned>, 4>{{{"vendor", 0x1F00},
                                                           {"renderer", 0x1F01},
                                                           {"version", 0x1F02},
                                                           {"glsl", 0x8B8C}}}) {
      const auto value = get_string(id);
      Log(std::string(name) + "=" +
          (value ? reinterpret_cast<const char *>(value) : "null"));
    }
  }

  void CheckWorkerContext(const CoreWindow &window) {
    Log("worker: creating shared context on UI thread");
    const auto create = Function<HANDLE(WINAPI *)(HDC, HANDLE, const int *)>(
        "wglCreateContextAttribsARB");
    constexpr int attributes[] = {0x2091, 4, 0x2092, 6, 0x9126, 1, 0};
    HANDLE shared = create(dc, context, attributes);
    if (!shared) {
      throw std::runtime_error("Shared context creation failed");
    }
    std::atomic<bool> done{false};
    std::exception_ptr failure;
    std::thread worker([&] {
      try {
        init_apartment(apartment_type::multi_threaded);
        Log("worker: activating shared context");
        if (!make_current(dc, shared)) {
          throw std::runtime_error("Worker context activation failed");
        }
        Log("worker: shared context active");
        CheckCompute();
        CheckEdenShaders();
        make_current(nullptr, nullptr);
        uninit_apartment();
      } catch (...) {
        failure = std::current_exception();
      }
      done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
      window.Dispatcher().ProcessEvents(
          CoreProcessEventsOption::ProcessAllIfPresent);
      Sleep(1);
    }
    worker.join();
    delete_context(shared);
    if (failure) {
      std::rethrow_exception(failure);
    }
    Log("WORKER_CONTEXT_PASS");
  }

  void CheckCompute() {
    const auto create_shader =
        Function<unsigned(WINAPI *)(unsigned)>("glCreateShader");
    const auto source = Function<void(WINAPI *)(
        unsigned, int, const char *const *, const int *)>("glShaderSource");
    const auto compile = Function<void(WINAPI *)(unsigned)>("glCompileShader");
    const auto shader_iv =
        Function<void(WINAPI *)(unsigned, unsigned, int *)>("glGetShaderiv");
    const auto shader_log =
        Function<void(WINAPI *)(unsigned, int, int *, char *)>(
            "glGetShaderInfoLog");
    const auto create_program =
        Function<unsigned(WINAPI *)()>("glCreateProgram");
    const auto attach =
        Function<void(WINAPI *)(unsigned, unsigned)>("glAttachShader");
    const auto link = Function<void(WINAPI *)(unsigned)>("glLinkProgram");
    const auto program_iv =
        Function<void(WINAPI *)(unsigned, unsigned, int *)>("glGetProgramiv");
    const auto program_log =
        Function<void(WINAPI *)(unsigned, int, int *, char *)>(
            "glGetProgramInfoLog");
    const auto use = Function<void(WINAPI *)(unsigned)>("glUseProgram");
    const auto gen_buffers =
        Function<void(WINAPI *)(int, unsigned *)>("glGenBuffers");
    const auto bind_buffer =
        Function<void(WINAPI *)(unsigned, unsigned)>("glBindBuffer");
    const auto buffer_data =
        Function<void(WINAPI *)(unsigned, ptrdiff_t, const void *, unsigned)>(
            "glBufferData");
    const auto bind_base =
        Function<void(WINAPI *)(unsigned, unsigned, unsigned)>(
            "glBindBufferBase");
    const auto dispatch =
        Function<void(WINAPI *)(unsigned, unsigned, unsigned)>(
            "glDispatchCompute");
    const auto barrier = Function<void(WINAPI *)(unsigned)>("glMemoryBarrier");
    const auto read =
        Function<void(WINAPI *)(unsigned, ptrdiff_t, ptrdiff_t, void *)>(
            "glGetBufferSubData");
    const auto delete_buffers =
        Function<void(WINAPI *)(int, const unsigned *)>("glDeleteBuffers");
    const auto delete_shader =
        Function<void(WINAPI *)(unsigned)>("glDeleteShader");
    const auto delete_program =
        Function<void(WINAPI *)(unsigned)>("glDeleteProgram");
    constexpr const char *code = R"(#version 450 core
layout(local_size_x=64) in;
layout(std430,binding=0) buffer Result { uint values[]; };
void main() { uint i=gl_GlobalInvocationID.x; values[i]=i*i+17u; }
)";
    Log("compute: creating shader");
    const unsigned shader = create_shader(0x91B9);
    source(shader, 1, &code, nullptr);
    Log("compute: compiling shader");
    compile(shader);
    Log("compute: compiled shader call returned");
    int compiled = 0;
    shader_iv(shader, 0x8B81, &compiled);
    if (!compiled) {
      std::array<char, 4096> message{};
      shader_log(shader, static_cast<int>(message.size()), nullptr,
                 message.data());
      delete_shader(shader);
      throw std::runtime_error(std::string("Compute compile failed: ") +
                               message.data());
    }
    const unsigned program = create_program();
    attach(program, shader);
    Log("compute: linking program");
    link(program);
    Log("compute: linked program call returned");
    delete_shader(shader);
    int linked = 0;
    program_iv(program, 0x8B82, &linked);
    if (!linked) {
      std::array<char, 4096> message{};
      program_log(program, static_cast<int>(message.size()), nullptr,
                  message.data());
      delete_program(program);
      throw std::runtime_error(std::string("Compute link failed: ") +
                               message.data());
    }
    unsigned buffer = 0;
    gen_buffers(1, &buffer);
    bind_buffer(0x90D2, buffer);
    std::array<unsigned, 64> result{};
    Log("compute: allocating storage");
    buffer_data(0x90D2, sizeof(result), result.data(), 0x88E8);
    bind_base(0x90D2, 0, buffer);
    use(program);
    Log("compute: dispatching");
    dispatch(1, 1, 1);
    Log("compute: dispatched");
    barrier(0x00000200);
    Log("compute: reading storage");
    read(0x90D2, 0, sizeof(result), result.data());
    delete_buffers(1, &buffer);
    use(0);
    delete_program(program);
    for (unsigned i = 0; i < result.size(); ++i) {
      if (result[i] != i * i + 17) {
        throw std::runtime_error("Compute readback mismatch at index " +
                                 std::to_string(i));
      }
    }
    Log("COMPUTE_READBACK_PASS count=64");
  }

  // Mirrors OpenGL::CreateProgram + LinkSeparableProgram for Eden's utility shaders.
  void CheckEdenShaders() {
    const auto create_shader =
        Function<unsigned(WINAPI *)(unsigned)>("glCreateShader");
    const auto source = Function<void(WINAPI *)(
        unsigned, int, const char *const *, const int *)>("glShaderSource");
    const auto compile = Function<void(WINAPI *)(unsigned)>("glCompileShader");
    const auto shader_iv =
        Function<void(WINAPI *)(unsigned, unsigned, int *)>("glGetShaderiv");
    const auto shader_log =
        Function<void(WINAPI *)(unsigned, int, int *, char *)>(
            "glGetShaderInfoLog");
    const auto create_program =
        Function<unsigned(WINAPI *)()>("glCreateProgram");
    const auto parameter = Function<void(WINAPI *)(unsigned, unsigned, int)>(
        "glProgramParameteri");
    const auto attach =
        Function<void(WINAPI *)(unsigned, unsigned)>("glAttachShader");
    const auto detach =
        Function<void(WINAPI *)(unsigned, unsigned)>("glDetachShader");
    const auto link = Function<void(WINAPI *)(unsigned)>("glLinkProgram");
    const auto program_iv =
        Function<void(WINAPI *)(unsigned, unsigned, int *)>("glGetProgramiv");
    const auto program_log =
        Function<void(WINAPI *)(unsigned, int, int *, char *)>(
            "glGetProgramInfoLog");
    const auto delete_shader =
        Function<void(WINAPI *)(unsigned)>("glDeleteShader");
    const auto delete_program =
        Function<void(WINAPI *)(unsigned)>("glDeleteProgram");
    unsigned failures = 0;
    for (const auto &[name, code] : kEdenShaders) {
      const std::string label(name);
      const unsigned shader = create_shader(0x91B9);
      const char *pointer = code.data();
      const int length = static_cast<int>(code.size());
      source(shader, 1, &pointer, &length);
      Log("eden: compiling " + label);
      compile(shader);
      int compiled = 0;
      shader_iv(shader, 0x8B81, &compiled);
      if (!compiled) {
        std::array<char, 2048> message{};
        shader_log(shader, static_cast<int>(message.size()), nullptr,
                   message.data());
        Log("EDEN_SHADER_COMPILE_FAIL " + label + ": " + message.data());
      }
      const unsigned program = create_program();
      parameter(program, 0x8258, 1);
      attach(program, shader);
      Log("eden: linking " + label);
      link(program);
      detach(program, shader);
      delete_shader(shader);
      int linked = 0;
      program_iv(program, 0x8B82, &linked);
      if (linked) {
        Log("EDEN_SHADER_PASS " + label);
      } else {
        ++failures;
        std::array<char, 2048> message{};
        program_log(program, static_cast<int>(message.size()), nullptr,
                    message.data());
        Log("EDEN_SHADER_LINK_FAIL " + label + ": " + message.data());
      }
      delete_program(program);
    }
    Log("EDEN_SHADERS_DONE failures=" + std::to_string(failures));
  }
};

struct Probe : implements<Probe, IFrameworkViewSource, IFrameworkView> {
  bool closed = false;
  IFrameworkView CreateView() { return *this; }
  void Initialize(const CoreApplicationView &view) {
    Log("view Initialize");
    view.Activated(
        [](const auto &,
           const Windows::ApplicationModel::Activation::IActivatedEventArgs &) {
          Log("view Activated");
          CoreWindow::GetForCurrentThread().Activate();
        });
  }
  void SetWindow(const CoreWindow &window) {
    window.Closed([this](const auto &, const auto &) { closed = true; });
  }
  void Load(const hstring &) {}
  void Uninitialize() {}
  void Run() {
    const auto window = CoreWindow::GetForCurrentThread();
    window.Activate();
    try {
      Log("BEGIN graphics probe; clear presentation is not gameplay FPS");
      Log("memory_limit=" +
          std::to_string(
              Windows::System::MemoryManager::AppMemoryUsageLimit()));
      CheckSparseMemory();
      Mesa mesa;
      mesa.Initialize(window);
      Log("context ready; beginning presentation");
      const auto clear_color =
          mesa.Function<void(WINAPI *)(float, float, float, float)>(
              "glClearColor");
      const auto clear = mesa.Function<void(WINAPI *)(unsigned)>("glClear");
      const auto swap = mesa.Function<BOOL(WINAPI *)(HDC)>("wglSwapBuffers");
      const auto interval =
          mesa.Function<BOOL(WINAPI *)(int)>("wglSwapIntervalEXT");
      const auto read = mesa.Function<void(WINAPI *)(
          int, int, int, int, unsigned, unsigned, void *)>("glReadPixels");
      const auto error = mesa.Function<unsigned(WINAPI *)()>("glGetError");
      interval(1);
      unsigned frames = 0;
      const auto start = std::chrono::steady_clock::now();
      while (!closed && std::chrono::steady_clock::now() - start <
                            std::chrono::seconds(60)) {
        window.Dispatcher().ProcessEvents(
            CoreProcessEventsOption::ProcessAllIfPresent);
        clear_color(0.0f, 0.5f, 0.25f, 1.0f);
        clear(0x00004000);
        if (frames == 0) {
          std::array<unsigned char, 4> pixel{};
          read(0, 0, 1, 1, 0x1908, 0x1401, pixel.data());
          if (pixel[0] > 2 || pixel[1] < 125 || pixel[1] > 130 ||
              pixel[2] < 62 || pixel[2] > 66) {
            throw std::runtime_error("Clear-color pixel readback mismatch");
          }
          Log("PIXEL_READBACK_PASS");
        }
        if (!swap(mesa.dc)) {
          throw std::runtime_error("Mesa swap failed");
        }
        if (error() != 0) {
          throw std::runtime_error("OpenGL error during presentation");
        }
        if (frames == 0) {
          Log("FIRST_PRESENT_PASS");
          mesa.CheckCompute();
          mesa.CheckWorkerContext(window);
        }
        ++frames;
      }
      const auto seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();
      Log("PRESENTATION_PASS frames=" + std::to_string(frames) +
          " seconds=" + std::to_string(seconds));
    } catch (const hresult_error &error) {
      Log("FAIL WinRT " + to_string(error.message()));
    } catch (const std::exception &error) {
      Log(std::string("FAIL ") + error.what());
    }
    Log("END graphics probe");
  }
};
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  try {
    init_apartment(apartment_type::multi_threaded);
    Log("entry point initialized MTA");
    CoreApplication::Run(make<Probe>());
  } catch (const hresult_error &error) {
    Log("FAIL startup WinRT " + to_string(error.message()));
    return 1;
  } catch (const std::exception &error) {
    Log(std::string("FAIL startup ") + error.what());
    return 1;
  }
  return 0;
}
