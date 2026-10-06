/* SPDX-License-Identifier: MIT
 * Shared lifetime accounting; implemented once in d3d12_screen.cpp.
 */
#pragma once
#include <atomic>
#include <stdint.h>

enum NxboxMetric { NxboxDxil, NxboxVariants, NxboxHeaps, NxboxHandles, NxboxCapacity, NxboxMetricCount };
extern std::atomic<long long> nxbox_metrics[NxboxMetricCount];
extern std::atomic<bool> nxbox_device_lost;
void nxbox_pso_created(ID3D12PipelineState *pso);
void nxbox_object_ref(ID3D12Object *object, int delta);
void nxbox_observe(ID3D12Device *dev, const char *stage, HRESULT api = S_OK);
void nxbox_pso_sample(ID3D12Device *dev, const char *kind, HRESULT api, HRESULT before,
                      HRESULT after, unsigned long long call, unsigned concurrent);
extern std::atomic<unsigned long long> nxbox_pso_calls;
extern std::atomic<unsigned> nxbox_pso_active;

/* Samples every attempt, including fallbacks and compute. A pre-existing loss is terminal. */
template <typename Create, typename Report>
HRESULT nxbox_create_pso(ID3D12Device *dev, const char *kind, Create create, Report report) {
   const auto call = ++nxbox_pso_calls;
   const unsigned concurrent = ++nxbox_pso_active;
   const HRESULT before = dev->GetDeviceRemovedReason();
   const HRESULT hr = SUCCEEDED(before) ? create() : before;
   const HRESULT after = dev->GetDeviceRemovedReason();
   report(before, hr, after);
   nxbox_pso_sample(dev, kind, hr, before, after, call, concurrent);
   --nxbox_pso_active;
   return FAILED(after) ? after : hr;
}

template <typename Create>
HRESULT nxbox_create_pso(ID3D12Device *dev, const char *kind, Create create) {
   return nxbox_create_pso(dev, kind, create, [](HRESULT, HRESULT, HRESULT) {});
}

#ifdef NXBOX_DRED_IMPLEMENTATION
#include <dxgi1_4.h>
#include <mutex>
#include <stdio.h>
#include <unordered_map>

std::atomic<long long> nxbox_metrics[NxboxMetricCount]{};
std::atomic<bool> nxbox_device_lost{false};
std::atomic<unsigned long long> nxbox_pso_calls{0};
std::atomic<unsigned> nxbox_pso_active{0};
namespace {
std::mutex nxbox_lifetime_mutex;
std::unordered_map<ID3D12Object *, unsigned> nxbox_psos;
unsigned long long nxbox_created = 0;
bool nxbox_first_pso_failure = false;

void nxbox_budget(ID3D12Device *dev, char *text, size_t size) {
   HRESULT hr = E_NOINTERFACE;
   DXGI_QUERY_VIDEO_MEMORY_INFO info{};
#ifndef _GAMING_XBOX
   /* Resolve the adapter by device LUID; never assume adapter zero or cast a DXCore screen. */
   auto module = util_dl_open("dxgi.dll");
   if (module) {
      using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
      auto create = (CreateFactory)util_dl_get_proc_address(module, "CreateDXGIFactory1");
      IDXGIFactory4 *factory = nullptr;
      IDXGIAdapter3 *adapter = nullptr;
      hr = create ? create(IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
      if (SUCCEEDED(hr)) {
         hr = factory->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
         if (SUCCEEDED(hr)) {
            hr = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
            adapter->Release();
         }
         factory->Release();
      }
      util_dl_close(module);
   }
#endif
   snprintf(text, size, "budget_hr=%08x local_usage=%llu local_budget=%llu reservation=%llu available=%llu",
            (unsigned)hr, (unsigned long long)info.CurrentUsage, (unsigned long long)info.Budget,
            (unsigned long long)info.CurrentReservation, (unsigned long long)info.AvailableForReservation);
}
} // namespace

void nxbox_pso_created(ID3D12PipelineState *pso) {
   std::lock_guard<std::mutex> lock(nxbox_lifetime_mutex);
   nxbox_psos[pso] = 1;
}
void nxbox_object_ref(ID3D12Object *object, int delta) {
   std::lock_guard<std::mutex> lock(nxbox_lifetime_mutex);
   auto it = nxbox_psos.find(object);
   if (it != nxbox_psos.end()) {
      if (delta > 0)
         ++it->second;
      else if (--it->second == 0)
         nxbox_psos.erase(it);
   }
}
void nxbox_observe(ID3D12Device *dev, const char *stage, HRESULT api) {
   HRESULT removed = dev->GetDeviceRemovedReason();
   char text[160];
   snprintf(text, sizeof(text), "stage=%s tid=%lu api=%08x removed=%08x tick=%llu", stage,
            GetCurrentThreadId(), (unsigned)api, (unsigned)removed, GetTickCount64());
   /* Preserve the earliest failed observation independently of periodic batch reporting. */
   if (!nxbox_device_lost.load(std::memory_order_relaxed))
      SetEnvironmentVariableA("NXBOX_D3D12_QUEUE", text);
   nxbox_dred_capture(dev, removed, stage);
}
void nxbox_pso_sample(ID3D12Device *dev, const char *kind, HRESULT api, HRESULT before,
                      HRESULT after, unsigned long long call, unsigned concurrent) {
   {
      std::lock_guard<std::mutex> lock(nxbox_lifetime_mutex);
      if (SUCCEEDED(api))
         ++nxbox_created;
      const bool first =
          (FAILED(api) || FAILED(before) || FAILED(after)) && !nxbox_first_pso_failure;
      if (first || (SUCCEEDED(api) && (nxbox_created % 8 == 0 || nxbox_created <= 4))) {
         char budget[240], text[1024];
         nxbox_budget(dev, budget, sizeof(budget));
         snprintf(text, sizeof(text),
                  "call=%llu created=%llu kind=%s tid=%lu concurrent=%u api=%08x before=%08x after=%08x "
                  "pso_alive=%zu dxil_alive=%lld variants=%lld heaps=%lld handles=%lld capacity=%lld %s",
                  call, nxbox_created, kind, GetCurrentThreadId(), concurrent, (unsigned)api,
                  (unsigned)before, (unsigned)after, nxbox_psos.size(), nxbox_metrics[NxboxDxil].load(),
                  nxbox_metrics[NxboxVariants].load(), nxbox_metrics[NxboxHeaps].load(),
                  nxbox_metrics[NxboxHandles].load(), nxbox_metrics[NxboxCapacity].load(), budget);
         SetEnvironmentVariableA("NXBOX_D3D12_RESOURCES", text);
         if (first) {
            nxbox_first_pso_failure = true;
            SetEnvironmentVariableA("NXBOX_D3D12_RESOURCES_FIRST", text);
         }
      }
   }
   nxbox_dred_capture(dev, FAILED(before) ? before : after, kind);
}
#endif
