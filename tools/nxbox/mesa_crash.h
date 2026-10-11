/* SPDX-License-Identifier: MIT
 * Direct failure evidence, independent of the environment journals and their
 * locks. Included only with the real Windows SDK (host COM mocks do not define
 * WINAPI_FAMILY).
 */
#pragma once

inline void nxbox_crash_gpu_error(const char *stage, HRESULT result,
                                  HRESULT removed) {
  if (SUCCEEDED(result) && SUCCEEDED(removed))
    return;
  using Callback = void(WINAPI *)(const char *, HRESULT, HRESULT);
  // Resolve on failure only. The executable stays loaded for the process
  // lifetime.
  const auto callback = reinterpret_cast<Callback>(
      GetProcAddress(GetModuleHandleW(nullptr), "NXboxCrashGpuError"));
  if (callback)
    callback(stage, result, removed);
}
