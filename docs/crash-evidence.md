# Crash evidence in the UWP frontend

`LocalState\nxbox_crash.txt` is an append-only, process-lifetime evidence file. Pull it together
with `eden_uwp_diag.txt` after a disappearance. Each record includes UTC FILETIME ticks (100 ns
since 1601), monotonic milliseconds, PID, TID, and main/gpu/cpu0–3/worker role. `process_start`
and `process_exit` delimit clean runs; an absent exit is evidence of an interrupted process,
not proof of a particular cause.

Handlers are installed at the first statement of `wWinMain`. The handle is opened immediately
following WinRT apartment initialization, before CoreApplication activation or worker creation.
Failures before that open go to OutputDebugString only. Native unhandled exceptions, terminate,
invalid parameters, pure calls, and SIGABRT write to the pre-opened append handle with WriteFile
and FlushFileBuffers. GENERIC_WRITE supplies the
[required flush access](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers);
[WriteFile's sentinel offsets](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile)
append without a shared seek position. The logger uses fixed stack storage, lock-free handle/role loads, and no
heap allocation or application mutex. It flushes the numeric exception reason before resolving
modules or walking stacks. Secondary faults in module resolution/stack capture are guarded.
Module lookup can still wait on the Windows loader; the numeric record is already persisted.

`fault` is the exception PC as module+RVA; access violations also include the target address and
access kind (0 read, 1 write, 8 execute). `handler_stack` is a bounded RtlCaptureStackBackTrace
of the logging caller, explicitly **not** a reconstructed CONTEXT unwind. Unknown/JIT addresses
are preserved as absolute addresses. Caught C++ exceptions are not observed/logged; native
unhandled C++ EH retains its 0xE06D7363 code. A terminate callback has no EXCEPTION_POINTERS and
records code 0 with its caller stack rather than inventing an exception code.

## UWP API boundary and unavoidable gaps

The requested first-priority VEH and stack guarantee cannot both be provided under the requirement
that every API be allowed in UWP AppContainer. Microsoft's current SDK restricts
[AddVectoredExceptionHandler](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/errhandlingapi.h)
and [SetThreadStackGuarantee](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/processthreadsapi.h)
to DESKTOP/SYSTEM/GAMES, excluding APP. They are compiled only for a desktop partition;
`uwp_no_vectored_handler_or_stack_guarantee` explicitly records their absence in UWP. There is no
attempt to bypass the partition with private declarations or dynamic imports. Stack-overflow
logging is therefore best effort, **not guaranteed**, including on emulated guest fiber stacks.

The SDK exposes GetModuleHandleExW (FROM_ADDRESS, UNCHANGED_REFCOUNT), GetModuleFileName,
GetModuleHandleW, and GetProcAddress in its
[APP partition](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/libloaderapi.h).
SetUnhandledExceptionFilter, RtlCaptureStackBackTrace, WriteFile, FlushFileBuffers, timestamps,
and CreateFile2FromAppW are covered by Microsoft's
[UWP API list](https://learn.microsoft.com/en-us/uwp/win32-and-com/win32-apis).
The file handle is opened inside ApplicationData.LocalFolder; no external path or broad-file
permission is required. CRT callbacks are standard runtime handlers; no unsupported
[_resetstkoflw or desktop CRT file/process API](https://learn.microsoft.com/en-us/cpp/cppcx/crt-functions-not-supported-in-universal-windows-platform-apps)
is used.

An actual [fail-fast](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raisefailfastexception)
bypasses frame and vectored handlers. The desktop VEH recognizes 0xC0000409/0xC0000374 if delivered
through normal exception dispatch, but cannot guarantee catching a runtime fast-fail. Our own CRT
callbacks persist their evidence before calling RaiseFailFastException. A hard OS kill also gives
no final callback. Neither this file nor the absence of an exception line proves an OS kill.

## Lifecycle, memory, and graphics

Process-lifetime subscriptions record Suspending, Resuming, EnteredBackground, LeavingBackground,
AppMemoryUsageIncreased, and AppMemoryUsageLimitChanging with usage/current/requested limits.
A one-second ThreadPoolTimer records usage independently of a blocked UI/game loop. It stops
executing during suspension. See the official
[lifecycle API](https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.core.coreapplication.enteredbackground)
and [MemoryManager](https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager).
Diagnostic() also flushes the OS file buffers after every line, including boot diagnostics.

Mesa's patched API boundary, lifetime observer, PSO attempts, and DRED removal entry call the
executable's exported NXboxCrashGpuError callback on failed HRESULTs/removal, before journal locks
and DRED queries. This works even with NXBOX_API_RING=0. It includes the stage, HRESULT, and actual
[GetDeviceRemovedReason](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getdeviceremovedreason)
result. Frontend DXGI presentation failures similarly record the D3D11 device-removal reason.
Rebuild/repackage **both frontend and patched Mesa** to receive immediate Mesa evidence; an old
Mesa DLL does not know the callback. The callback only records; existing recovery/shutdown policy
still owns game termination.

## Verification

`tests/port/test_crash_format.py` compiles the std-only formatting/code-selection header on the
host and checks module basenames/RVAs, unresolved addresses, integer boundaries, control-character
sanitization, truncation/newline integrity, and exclusion of handled C++ EH/breakpoints/guard pages.
Native callback delivery, stack exhaustion, lifecycle transitions, and driver loss require a
Windows UWP build and console testing; host formatting tests do not establish those behaviors.
