/* SPDX-License-Identifier: MIT
 * One process-wide completion ring, shared by every Mesa worker and queue.
 * The hot path allocates nothing and never waits for another producer. Slots
 * use atomic bytes and a generation stamp: even a stalled/overlapping writer
 * cannot cause a data race or a torn diagnostic. Contention is reported.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

inline bool nxbox_api_enabled() {
  static const bool enabled = [] {
    // The ring formats a line for every D3D12 call (tens of thousands a second): it is off in the app unless NXBOX_API_RING=1.
    char value[4]{};
    if (GetEnvironmentVariableA("NXBOX_SYNC_BATCH", value, sizeof(value)) == 1 &&
        value[0] == '0')
      return false;
    // The frontend sets NXBOX_API_RING=0 unless the user asked for the ring.
    return !(GetEnvironmentVariableA("NXBOX_API_RING", value, sizeof(value)) == 1 &&
             value[0] == '0');
  }();
  return enabled;
}
// Shared with the batch guards. A failed Close/Reset is terminal even when
// synchronous diagnostics are disabled; never record into the poisoned list.
inline std::atomic<bool> &nxbox_command_stopped() {
  static std::atomic<bool> stopped{false};
  return stopped;
}
struct NxboxApiRing {
  static constexpr unsigned capacity = 256, line_size = 1024;
  struct Slot {
    std::atomic<uint64_t> stamp{0};
    std::atomic<char> text[line_size]{};
  } slots[capacity];
  std::atomic<uint64_t> next{0};
  // Process lifetime creation totals, NOT live bytes or residency usage.
  std::atomic<uint64_t> custom_created{0}, custom_bytes{0}, custom_unknown{0};
  // Live resource objects: wrapped by d3d12_bo_wrap_res, dropped when the bo is destroyed.
  std::atomic<int64_t> bo_live{0}, bo_live_bytes{0};
  std::atomic<uint64_t> bo_total{0};
  // 0 = live, 1 = publishing, 2 = immutable committed capture.
  std::atomic<unsigned> captured{0};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free &&
                  std::atomic<char>::is_always_lock_free,
              "The API ring requires lock-free atomics");
inline NxboxApiRing &nxbox_api_ring() {
  static NxboxApiRing ring;
  return ring;
}
inline void nxbox_api_record(ID3D12Device *dev, const char *call,
                             const char *site, const char *args, HRESULT result,
                             HRESULT removed, bool command_failure = false) {
  if (!nxbox_api_enabled())
    return;
  auto &ring = nxbox_api_ring();
  if (ring.captured.load(std::memory_order_acquire))
    return;
  const auto seq = ring.next.fetch_add(1);
  {
    // Call-type histogram per 25000 recorded calls, to see what a slow phase is made of.
    // Diagnostic only: racy increments may lose a count.
    struct Count { const char *name; unsigned long long n; };
    static Count counts[64];
    static unsigned used = 0;
    unsigned i = 0;
    while (i < used && counts[i].name != call)
      ++i;
    if (i == used && used < 64) {
      counts[used].name = call;
      i = used++;
    }
    if (i < 64)
      ++counts[i].n;
    if (seq % 25000 == 24999) {
      char text[1024];
      int length = 0;
      for (unsigned j = 0; j < used && length < (int)sizeof(text) - 48; ++j) {
        length += snprintf(text + length, sizeof(text) - length, "%s=%llu ", counts[j].name,
                           counts[j].n);
        counts[j].n = 0;
      }
      SetEnvironmentVariableA("NXBOX_D3D12_CALLS", text);
    }
  }
  char line[NxboxApiRing::line_size];
  snprintf(line, sizeof(line),
           "seq=%llu ms=%llu tid=%lu dev=%p call=%s site=%s hr=0x%08x "
           "removed=0x%08x %s custom_created=%llu custom_bytes=%llu "
           "custom_unknown=%llu bo_live=%lld bo_live_bytes=%lld bo_total=%llu",
           (unsigned long long)seq, (unsigned long long)GetTickCount64(),
           (unsigned long)GetCurrentThreadId(), (void *)dev, call, site,
           (unsigned)result, (unsigned)removed, args,
           (unsigned long long)ring.custom_created.load(),
           (unsigned long long)ring.custom_bytes.load(),
           (unsigned long long)ring.custom_unknown.load(),
           (long long)ring.bo_live.load(), (long long)ring.bo_live_bytes.load(),
           (unsigned long long)ring.bo_total.load());
  auto &slot = ring.slots[seq % NxboxApiRing::capacity];
  auto stamp = slot.stamp.load();
  // One attempt, no spinlock. A busy slot becomes an explicit gap on capture.
  if (!(stamp & 1) && stamp < (seq + 1) * 2 &&
      slot.stamp.compare_exchange_strong(stamp, (seq + 1) * 2 - 1)) {
    unsigned i = 0;
    do {
      slot.text[i].store(line[i], std::memory_order_relaxed);
    } while (line[i++]);
    slot.stamp.store((seq + 1) * 2);
  }
  unsigned live = 0;
  if ((removed == S_OK && !command_failure) ||
      !ring.captured.compare_exchange_strong(live, 1))
    return;
  // Freeze the last 256 reserved completions. Always preserve the winner's
  // full line separately, including if its slot was busy at wraparound.
  const auto end = ring.next.load();
  const auto begin =
      end > NxboxApiRing::capacity ? end - NxboxApiRing::capacity : 0;
  unsigned gaps = 0;
  for (auto n = begin; n < end; ++n) {
    auto &entry = ring.slots[n % NxboxApiRing::capacity];
    char text[NxboxApiRing::line_size]{};
    const auto expected = (n + 1) * 2;
    const auto before = entry.stamp.load();
    for (unsigned i = 0; i < sizeof(text) - 1; ++i) {
      text[i] = entry.text[i].load(std::memory_order_relaxed);
      if (!text[i])
        break;
    }
    if (before != expected || entry.stamp.load() != expected) {
      ++gaps;
      snprintf(text, sizeof(text),
               "seq=%llu unavailable=in-progress-or-overwritten",
               (unsigned long long)n);
    }
    char key[64];
    snprintf(key, sizeof(key), "NXBOX_D3D12_API_RING_%u",
             (unsigned)(n - begin));
    SetEnvironmentVariableA(key, text);
  }
  SetEnvironmentVariableA("NXBOX_D3D12_API_FIRST", line);
  char manifest[256];
  snprintf(manifest, sizeof(manifest),
           "parts=%u first_seq=%llu first_call=%s removed=0x%08x gaps=%u "
           "attribution=%s",
           (unsigned)(end - begin), (unsigned long long)seq, call,
           (unsigned)removed, gaps,
           command_failure ? "command-api-failure"
                           : "first-observed-after-call");
  SetEnvironmentVariableA("NXBOX_D3D12_API_RING", manifest);
  ring.captured.store(2, std::memory_order_release);
}
// Every existing loss observer calls this BEFORE publishing DEVICE_LOST.
// Another observer may win while a producer is committing the environment.
inline void nxbox_api_capture(ID3D12Device *dev, HRESULT removed,
                              const char *site) {
  if (!nxbox_api_enabled() || removed == S_OK)
    return;
  nxbox_api_record(dev, "observation", site, "phase=external-observer", removed,
                   removed);
  while (nxbox_api_ring().captured.load(std::memory_order_acquire) == 1)
    Sleep(0);
}

#ifndef NXBOX_API_RING_CORE_ONLY
void nxbox_dred_capture(ID3D12Device *, HRESULT, const char *, const char *);
// Obtain the owning device, including video queues/fences and newer interfaces.
// Device calls need no QI. Child references live only for the call, never in
// the ring.
template <typename T> T *nxbox_api_raw(T *value) { return value; }
template <typename T>
auto nxbox_api_raw(const T &value) -> decltype(value.Get()) {
  return value.Get();
}
struct NxboxApiDevice {
  ID3D12Device *dev = nullptr;
  bool release = false;
  template <typename T> explicit NxboxApiDevice(T *object) {
    if constexpr (std::is_base_of<ID3D12Device, T>::value)
      dev = object;
    else if constexpr (std::is_base_of<ID3D12DeviceChild, T>::value) {
      release = SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&dev)));
    } else {
      release = SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&dev)));
    }
  }
  ~NxboxApiDevice() {
    if (release)
      dev->Release();
  }
};
struct NxboxApiArgs {
  char text[640]{};
  unsigned used = 0;
  bool custom = false, has_resource = false;
  D3D12_RESOURCE_DESC allocation{};
  template <typename... A> void add(const char *fmt, A... args) {
    if (used >= sizeof(text) - 1)
      return;
    int n = snprintf(text + used, sizeof(text) - used, fmt, args...);
    if (n > 0)
      used += (unsigned)n < sizeof(text) - used ? (unsigned)n
                                                : sizeof(text) - used - 1;
  }
  template <typename T> void value(const T &v) {
    if constexpr (std::is_integral<T>::value || std::is_enum<T>::value)
      add("%llu ", (unsigned long long)v);
    else if constexpr (std::is_pointer<T>::value)
      add("%p ", (const void *)v);
    else
      add("%s", "object ");
  }
  template <typename D> void resource(const D *d) {
    if (!d) {
      add("%s", "resource=NULL ");
      return;
    }
    has_resource = true;
    allocation.Dimension = d->Dimension;
    allocation.Alignment = d->Alignment;
    allocation.Width = d->Width;
    allocation.Height = d->Height;
    allocation.DepthOrArraySize = d->DepthOrArraySize;
    allocation.MipLevels = d->MipLevels;
    allocation.Format = d->Format;
    allocation.SampleDesc = d->SampleDesc;
    allocation.Layout = d->Layout;
    allocation.Flags = d->Flags;
    add("resource={size=%llux%ux%u format=%u flags=%x dimension=%u mips=%u "
        "samples=%u} ",
        (unsigned long long)d->Width, d->Height, (unsigned)d->DepthOrArraySize,
        (unsigned)d->Format, (unsigned)d->Flags, (unsigned)d->Dimension,
        (unsigned)d->MipLevels, d->SampleDesc.Count);
  }
  void value(const D3D12_RESOURCE_DESC *d) { resource(d); }
  void value(D3D12_RESOURCE_DESC *d) { resource(d); }
  void value(const D3D12_RESOURCE_DESC1 *d) { resource(d); }
  void value(D3D12_RESOURCE_DESC1 *d) { resource(d); }
  // Views: the resource's own description next to the view's, so a mismatch is visible.
  void value(ID3D12Resource *r) {
    add("%p ", (const void *)r);
    if (r) {
      const D3D12_RESOURCE_DESC d = r->GetDesc();
      add("res={fmt=%u dim=%u %llux%ux%u mips=%u flags=%x} ", (unsigned)d.Format,
          (unsigned)d.Dimension, (unsigned long long)d.Width, d.Height,
          (unsigned)d.DepthOrArraySize, (unsigned)d.MipLevels, (unsigned)d.Flags);
    }
  }
  void value(const D3D12_SHADER_RESOURCE_VIEW_DESC *d) {
    if (!d) {
      add("%s", "srv=NULL ");
      return;
    }
    add("srv={fmt=%u dim=%u map=%x mip=%u levels=%u slice=%u count=%u} ", (unsigned)d->Format,
        (unsigned)d->ViewDimension, (unsigned)d->Shader4ComponentMapping,
        d->Texture2DArray.MostDetailedMip, d->Texture2DArray.MipLevels,
        d->Texture2DArray.FirstArraySlice, d->Texture2DArray.ArraySize);
  }
  void value(D3D12_SHADER_RESOURCE_VIEW_DESC *d) { value((const D3D12_SHADER_RESOURCE_VIEW_DESC *)d); }
  void value(const D3D12_RENDER_TARGET_VIEW_DESC *d) {
    if (!d) {
      add("%s", "rtv=NULL ");
      return;
    }
    add("rtv={fmt=%u dim=%u mip=%u slice=%u count=%u} ", (unsigned)d->Format,
        (unsigned)d->ViewDimension, d->Texture2DArray.MipSlice, d->Texture2DArray.FirstArraySlice,
        d->Texture2DArray.ArraySize);
  }
  void value(D3D12_RENDER_TARGET_VIEW_DESC *d) { value((const D3D12_RENDER_TARGET_VIEW_DESC *)d); }
  void value(const D3D12_DEPTH_STENCIL_VIEW_DESC *d) {
    if (!d) {
      add("%s", "dsv=NULL ");
      return;
    }
    add("dsv={fmt=%u dim=%u flags=%u mip=%u slice=%u count=%u} ", (unsigned)d->Format,
        (unsigned)d->ViewDimension, (unsigned)d->Flags, d->Texture2DArray.MipSlice,
        d->Texture2DArray.FirstArraySlice, d->Texture2DArray.ArraySize);
  }
  void value(D3D12_DEPTH_STENCIL_VIEW_DESC *d) { value((const D3D12_DEPTH_STENCIL_VIEW_DESC *)d); }
  void value(const D3D12_UNORDERED_ACCESS_VIEW_DESC *d) {
    if (!d) {
      add("%s", "uav=NULL ");
      return;
    }
    add("uav={fmt=%u dim=%u mip=%u slice=%u count=%u} ", (unsigned)d->Format,
        (unsigned)d->ViewDimension, d->Texture2DArray.MipSlice, d->Texture2DArray.FirstArraySlice,
        d->Texture2DArray.ArraySize);
  }
  void value(D3D12_UNORDERED_ACCESS_VIEW_DESC *d) { value((const D3D12_UNORDERED_ACCESS_VIEW_DESC *)d); }
  void value(const D3D12_HEAP_PROPERTIES *p) {
    if (p) {
      custom = p->Type == D3D12_HEAP_TYPE_CUSTOM;
      add("heap_type=%u cpu_page=%u pool=%u ", (unsigned)p->Type,
          (unsigned)p->CPUPageProperty, (unsigned)p->MemoryPoolPreference);
    }
  }
  void value(D3D12_HEAP_PROPERTIES *p) {
    value((const D3D12_HEAP_PROPERTIES *)p);
  }
  void value(const D3D12_HEAP_DESC *d) {
    if (d) {
      add("heap_size=%llu heap_flags=%x ", (unsigned long long)d->SizeInBytes,
          (unsigned)d->Flags);
      value(&d->Properties);
    }
  }
  void value(D3D12_HEAP_DESC *d) { value((const D3D12_HEAP_DESC *)d); }
  void value(const D3D12_DESCRIPTOR_HEAP_DESC *d) {
    if (d)
      add("descriptors=%u type=%u flags=%x ", d->NumDescriptors,
          (unsigned)d->Type, (unsigned)d->Flags);
  }
  void value(D3D12_DESCRIPTOR_HEAP_DESC *d) {
    value((const D3D12_DESCRIPTOR_HEAP_DESC *)d);
  }
  void value(D3D12_GPU_DESCRIPTOR_HANDLE v) {
    add("gpu=%llu ", (unsigned long long)v.ptr);
  }
  void value(D3D12_CPU_DESCRIPTOR_HANDLE v) {
    add("cpu=%llu ", (unsigned long long)v.ptr);
  }
};
#ifndef NXBOX_API_RING_NO_LIST
#include "nxbox_list_ring.h"
#endif
// Journal helpers otherwise hide the Gallium origin behind a helper-header
// line. Scope attribution on this thread without retaining a context pointer.
inline const char *&nxbox_api_origin() {
  static thread_local const char *site = nullptr;
  return site;
}
struct NxboxApiSiteScope {
  const char *previous;
  explicit NxboxApiSiteScope(const char *site) : previous(nxbox_api_origin()) {
    nxbox_api_origin() = site;
  }
  ~NxboxApiSiteScope() { nxbox_api_origin() = previous; }
};
#define NXBOX_API_SITE_SCOPE(site) NxboxApiSiteScope nxbox_api_site_scope(site)
template <typename T> struct NxboxApi {
  T *object;
  const char *site;
  template <typename Call, typename... A>
  auto invoke(const char *name, Call call, const A &...args) {
    using Result = decltype(call());
#ifndef NXBOX_API_RING_NO_LIST
    constexpr bool list_object = std::is_base_of<ID3D12CommandList, T>::value;
#else
    constexpr bool list_object = false;
#endif
    constexpr bool command_object =
        list_object || std::is_base_of<ID3D12GraphicsCommandList, T>::value ||
        std::is_base_of<ID3D12CommandAllocator, T>::value ||
        std::is_base_of<ID3D12CommandQueue, T>::value;
    if constexpr (command_object) {
      // Signals/waits may still be needed to drain work submitted before
      // failure.
      if (nxbox_command_stopped().load() && strcmp(name, "Signal") &&
          strcmp(name, "Wait")
#ifndef NXBOX_API_RING_NO_LIST
          && !(list_object && !strcmp(name, "Close") &&
               nxbox_env_flag("NXBOX_D3D12_DUMP_EVERY_CLOSE"))
#endif
      ) {
        if constexpr (std::is_void<Result>::value)
          return;
        else
          return E_INVALIDARG;
      }
    }
#ifndef NXBOX_API_RING_NO_LIST
    if constexpr (list_object) {
      nxbox_validate(name, site, args...);
      NxboxListRef journal(object);
      if (journal.p) {
        std::lock_guard<std::mutex> lock(journal.p->mutex);
        journal.p->record(site, name, args...);
      }
    }
    if constexpr (sizeof...(A) >= 3) {
      const auto tuple = std::tie(args...);
      if constexpr (std::is_same<std::decay_t<decltype(std::get<sizeof...(A) -
                                                                1>(tuple))>,
                                 D3D12_CPU_DESCRIPTOR_HANDLE>::value &&
                    std::is_convertible<decltype(std::get<0>(tuple)),
                                        ID3D12Resource *>::value) {
        if (!strcmp(name, "CreateRenderTargetView") ||
            !strcmp(name, "CreateDepthStencilView"))
          nxbox_view_created(std::get<0>(tuple),
                             std::get<sizeof...(A) - 1>(tuple));
      }
    }
#endif
    const bool enabled = nxbox_api_enabled();
    // Close/Reset failure handling is safety-critical, independent of the ring.
    if (!enabled && !list_object && strcmp(name, "Close") &&
        strcmp(name, "Reset") && strncmp(name, "CreateCommandList", 17) &&
        strcmp(name, "CreateDescriptorHeap"))
      return call();
    NxboxApiDevice device(object);
    NxboxApiArgs details;
    details.add("object=%p args=", (void *)object);
    unsigned index = 0;
    auto argument = [&](const auto &arg) {
      const char *label = nullptr;
      if ((!strncmp(name, "CreateCommittedResource", 23) ||
           !strncmp(name, "CreatePlacedResource", 20)) &&
          index == 3)
        label = "initial_state_or_layout";
      if (!strncmp(name, "CreateCommittedResource", 23) && index == 1)
        label = "heap_flags";
      if (!strncmp(name, "CreatePlacedResource", 20) && index == 1)
        label = "heap_offset";
      if ((!strcmp(name, "Signal") || !strcmp(name, "Wait")) && index == 1)
        label = "fence_value";
      if (!strcmp(name, "SetEventOnCompletion") && index == 0)
        label = "fence_value";
      if (!strcmp(name, "CopyDescriptors") && index >= 7)
        label = index == 7 ? "dst_descriptors" : "src_descriptors";
      if (!strcmp(name, "CopyBufferRegion")) {
        static const char *labels[]{"dst",        "dst_offset", "src",
                                    "src_offset", "bytes",      "dst_size",
                                    "src_size"};
        if (index < 7)
          label = labels[index];
      }
      if (label)
        details.add("%s=", label);
      else
        details.add("arg%u=", index);
      details.value(arg);
      if constexpr (std::is_integral<std::decay_t<decltype(arg)>>::value ||
                    std::is_enum<std::decay_t<decltype(arg)>>::value) {
        if (label && !strcmp(label, "heap_flags"))
          details.add("heap_flags_hex=0x%x not_resident=%u not_zeroed=%u ",
                      (unsigned)arg, ((unsigned)arg & 0x800) != 0,
                      ((unsigned)arg & 0x1000) != 0);
      }
      ++index;
    };
    (argument(args), ...);
    UINT64 custom_size = UINT64_MAX;
    const bool custom_commit = enabled && details.custom &&
                               details.has_resource &&
                               !strncmp(name, "CreateCommittedResource", 23);
    if (custom_commit && device.dev) {
#if defined(_WIN32) && !defined(_MSC_VER)
      D3D12_RESOURCE_ALLOCATION_INFO info{};
      device.dev->GetResourceAllocationInfo(&info, 0, 1, &details.allocation);
#else
      const auto info =
          device.dev->GetResourceAllocationInfo(0, 1, &details.allocation);
#endif
      custom_size = info.SizeInBytes;
      details.add("allocation_bytes=%llu ", (unsigned long long)custom_size);
    }
    auto finish = [&](HRESULT result) {
#ifndef NXBOX_API_RING_NO_LIST
      if constexpr (sizeof...(A) == 3) {
        if (result >= 0 && !strcmp(name, "CreateDescriptorHeap")) {
          const auto tuple = std::tie(args...);
          const auto &output = std::get<2>(tuple);
          if constexpr (std::is_pointer<std::decay_t<decltype(output)>>::value)
            nxbox_heap_created(
                device.dev,
                output
                    ? *reinterpret_cast<ID3D12DescriptorHeap *const *>(output)
                    : nullptr);
        }
      }
      if constexpr (sizeof...(A) == 6 || sizeof...(A) == 5) {
        if (result >= 0 && !strncmp(name, "CreateCommandList", 17)) {
          const auto tuple = std::tie(args...);
          const auto &output = std::get<sizeof...(A) - 1>(tuple);
          if constexpr (std::is_pointer<
                            std::decay_t<decltype(output)>>::value) {
            nxbox_list_created(
                output ? *reinterpret_cast<IUnknown *const *>(output) : nullptr,
                site, details.text);
          }
        }
      }
      if constexpr (list_object) {
        if (!strcmp(name, "Reset") && result >= 0) {
          NxboxListRef journal(object);
          if (journal.p) {
            std::lock_guard<std::mutex> lock(journal.p->mutex);
            journal.p->fragments = 0;
            journal.p->next = 0;
            ++journal.p->generation;
            journal.p->record(site, name, args...);
          }
        }
      }
#endif
      const bool command_failure =
          command_object && result < 0 &&
          (!strcmp(name, "Close") || !strcmp(name, "Reset"));
      const bool first_command_failure =
          command_failure && !nxbox_command_stopped().exchange(true);
#ifndef NXBOX_API_RING_NO_LIST
      if constexpr (list_object) {
        if (command_failure && (first_command_failure ||
                                nxbox_env_flag("NXBOX_D3D12_DUMP_EVERY_CLOSE")))
          nxbox_list_failure(object, device.dev, name, site, result);
      }
#endif
      if (custom_commit && result == S_OK) {
        auto &ring = nxbox_api_ring();
        ++ring.custom_created;
        if (custom_size == UINT64_MAX)
          ++ring.custom_unknown;
        else
          ring.custom_bytes.fetch_add(custom_size);
      }
      if (!enabled && strcmp(name, "Close") && strcmp(name, "Reset"))
        return;
      if (!device.dev)
        return;
      const HRESULT removed = device.dev->GetDeviceRemovedReason();
      nxbox_api_record(device.dev, name, site, details.text, result, removed,
                       command_failure);
      if (first_command_failure) {
        while (nxbox_api_ring().captured.load(std::memory_order_acquire) == 1)
          Sleep(0);
        char failure[192];
        snprintf(failure, sizeof(failure),
                 "call=%s site=%s hr=0x%08x removed=0x%08x; recording stopped",
                 name, site, (unsigned)result, (unsigned)removed);
        SetEnvironmentVariableA("NXBOX_D3D12_SYNC_ERROR", failure);
        // Publish only after the ring manifest, so the frontend can drain it.
        SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", "1");
      }
      nxbox_dred_capture(device.dev, removed, name, nullptr);
    };
    if constexpr (std::is_void<Result>::value) {
      call();
      finish(S_OK);
    } else {
      const auto result = call();
      finish(result);
      return result;
    }
  }
#define NXBOX_API_METHOD(name)                                                 \
  template <typename... A> auto name(A &&...args) {                            \
    return invoke(                                                             \
        #name, [&]() { return object->name(std::forward<A>(args)...); },       \
        args...);                                                              \
  }
  NXBOX_API_METHOD(IASetPrimitiveTopology)
  NXBOX_API_METHOD(RSSetViewports)
  NXBOX_API_METHOD(RSSetScissorRects)
  NXBOX_API_METHOD(OMSetBlendFactor)
  NXBOX_API_METHOD(OMSetStencilRef)
  NXBOX_API_METHOD(OMSetFrontAndBackStencilRef)
  NXBOX_API_METHOD(SetComputeRoot32BitConstant)
  NXBOX_API_METHOD(SetGraphicsRoot32BitConstant)
  NXBOX_API_METHOD(SetComputeRootConstantBufferView)
  NXBOX_API_METHOD(SetGraphicsRootConstantBufferView)
  NXBOX_API_METHOD(SetComputeRootShaderResourceView)
  NXBOX_API_METHOD(SetGraphicsRootShaderResourceView)
  NXBOX_API_METHOD(SetComputeRootUnorderedAccessView)
  NXBOX_API_METHOD(SetGraphicsRootUnorderedAccessView)
  NXBOX_API_METHOD(ClearUnorderedAccessViewUint)
  NXBOX_API_METHOD(ClearUnorderedAccessViewFloat)
  NXBOX_API_METHOD(DiscardResource)
  NXBOX_API_METHOD(SetMarker)
  NXBOX_API_METHOD(BeginEvent)
  NXBOX_API_METHOD(EndEvent)
  NXBOX_API_METHOD(CopyTiles)
  NXBOX_API_METHOD(ResolveSubresourceRegion)
  NXBOX_API_METHOD(SetSamplePositions)
  NXBOX_API_METHOD(SetViewInstanceMask)
  NXBOX_API_METHOD(SetProtectedResourceSession)
  NXBOX_API_METHOD(ExecuteCommandLists)
  NXBOX_API_METHOD(CreateCommittedResource)
  NXBOX_API_METHOD(CreateCommittedResource1)
  NXBOX_API_METHOD(CreateCommittedResource2)
  NXBOX_API_METHOD(CreateCommittedResource3)
  NXBOX_API_METHOD(CreatePlacedResource)
  NXBOX_API_METHOD(CreatePlacedResource1)
  NXBOX_API_METHOD(CreatePlacedResource2)
  NXBOX_API_METHOD(CreateHeap)
  NXBOX_API_METHOD(CreateHeap1)
  NXBOX_API_METHOD(MakeResident)
  NXBOX_API_METHOD(EnqueueMakeResident)
  NXBOX_API_METHOD(Evict)
  NXBOX_API_METHOD(CreateDescriptorHeap)
  void CopyDescriptors(UINT dst_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *dst,
                       const UINT *dst_sizes, UINT src_ranges,
                       const D3D12_CPU_DESCRIPTOR_HANDLE *src,
                       const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    UINT64 dst_count = 0, src_count = 0;
    if (nxbox_api_enabled()) {
      for (UINT i = 0; i < dst_ranges; ++i)
        dst_count += dst_sizes ? dst_sizes[i] : 1;
      for (UINT i = 0; i < src_ranges; ++i)
        src_count += src_sizes ? src_sizes[i] : 1;
    }
    invoke(
        "CopyDescriptors",
        [&] {
          object->CopyDescriptors(dst_ranges, dst, dst_sizes, src_ranges, src,
                                  src_sizes, type);
        },
        dst_ranges, dst, dst_sizes, src_ranges, src, src_sizes, type, dst_count,
        src_count);
  }
  NXBOX_API_METHOD(CopyDescriptorsSimple)
  NXBOX_API_METHOD(Signal)
  NXBOX_API_METHOD(Wait)
  NXBOX_API_METHOD(SetEventOnCompletion)
  NXBOX_API_METHOD(Reset)
  NXBOX_API_METHOD(Close)
  NXBOX_API_METHOD(SetDescriptorHeaps)
  NXBOX_API_METHOD(SetGraphicsRootDescriptorTable)
  NXBOX_API_METHOD(SetComputeRootDescriptorTable)
  // Use subtraction after checking the offset, so UINT64 overflow cannot
  // turn an out-of-bounds copy into an apparently valid end offset.
  void CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset,
                        ID3D12Resource *src, UINT64 src_offset, UINT64 bytes) {
#ifndef NXBOX_API_RING_NO_LIST
    nxbox_validate("CopyBufferRegion", site, dst, dst_offset, src, src_offset,
                   bytes);
#endif
    const UINT64 dst_size = dst ? dst->GetDesc().Width : 0;
    const UINT64 src_size = src ? src->GetDesc().Width : 0;
    const bool valid =
        dst && src && dst_offset <= dst_size && src_offset <= src_size &&
        bytes <= dst_size - dst_offset && bytes <= src_size - src_offset;
    if (!valid) {
      char text[256];
      snprintf(text, sizeof(text),
               "CopyBufferRegion rejected dst_size=%llu dst_offset=%llu "
               "src_size=%llu src_offset=%llu bytes=%llu",
               (unsigned long long)dst_size, (unsigned long long)dst_offset,
               (unsigned long long)src_size, (unsigned long long)src_offset,
               (unsigned long long)bytes);
      SetEnvironmentVariableA("NXBOX_D3D12_SYNC_ERROR", text);
      invoke(
          "CopyBufferRegion-rejected", [] { return E_INVALIDARG; }, dst_size,
          dst_offset, src_size, src_offset, bytes);
      return;
    }
    invoke(
        "CopyBufferRegion",
        [&] {
          object->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes);
        },
        dst, dst_offset, src, src_offset, bytes, dst_size, src_size);
  }
  NXBOX_API_METHOD(CopyTextureRegion)
  NXBOX_API_METHOD(CopyResource)
  NXBOX_API_METHOD(ResourceBarrier)
  NXBOX_API_METHOD(ResolveQueryData)
  NXBOX_API_METHOD(ExecuteBundle)
  NXBOX_API_METHOD(CreateCommandQueue)
  NXBOX_API_METHOD(CreateCommandAllocator)
  NXBOX_API_METHOD(CreateCommandList)
  NXBOX_API_METHOD(CreateFence)
  NXBOX_API_METHOD(CreateQueryHeap)
  NXBOX_API_METHOD(CreateRootSignature)
  NXBOX_API_METHOD(CreateCommandSignature)
  NXBOX_API_METHOD(CreateShaderResourceView)
  NXBOX_API_METHOD(CreateUnorderedAccessView)
  NXBOX_API_METHOD(CreateRenderTargetView)
  NXBOX_API_METHOD(CreateDepthStencilView)
  NXBOX_API_METHOD(CreateConstantBufferView)
  NXBOX_API_METHOD(CreateSampler)
  NXBOX_API_METHOD(Map)
  NXBOX_API_METHOD(Unmap)
  NXBOX_API_METHOD(OpenSharedHandle)
  NXBOX_API_METHOD(CreateSharedHandle)
  NXBOX_API_METHOD(OpenSharedHandleByName)
  NXBOX_API_METHOD(CreateCommandQueue1)
  NXBOX_API_METHOD(CreateCommandList1)
  NXBOX_API_METHOD(CreateGraphicsPipelineState)
  NXBOX_API_METHOD(CreateComputePipelineState)
  NXBOX_API_METHOD(CreatePipelineState)
  NXBOX_API_METHOD(CreateVideoDecoder)
  NXBOX_API_METHOD(CreateVideoDecoderHeap)
  NXBOX_API_METHOD(CreateVideoEncoder)
  NXBOX_API_METHOD(CreateVideoEncoderHeap)
  NXBOX_API_METHOD(CreateVideoProcessor)
  NXBOX_API_METHOD(DecodeFrame1)
  NXBOX_API_METHOD(ProcessFrames1)
  NXBOX_API_METHOD(EncodeFrame)
  NXBOX_API_METHOD(ResolveEncoderOutputMetadata)
  NXBOX_API_METHOD(BeginQuery)
  NXBOX_API_METHOD(EndQuery)
  NXBOX_API_METHOD(WriteBufferImmediate)
  NXBOX_API_METHOD(ExecuteIndirect)
  NXBOX_API_METHOD(DrawInstanced)
  NXBOX_API_METHOD(DrawIndexedInstanced)
  NXBOX_API_METHOD(Dispatch)
  NXBOX_API_METHOD(ResolveSubresource)
  NXBOX_API_METHOD(ClearRenderTargetView)
  NXBOX_API_METHOD(ClearDepthStencilView)
  NXBOX_API_METHOD(SetPipelineState)
  NXBOX_API_METHOD(SetGraphicsRootSignature)
  NXBOX_API_METHOD(SetComputeRootSignature)
  NXBOX_API_METHOD(SetGraphicsRoot32BitConstants)
  NXBOX_API_METHOD(SetComputeRoot32BitConstants)
  NXBOX_API_METHOD(SetPredication)
  NXBOX_API_METHOD(IASetVertexBuffers)
  NXBOX_API_METHOD(IASetIndexBuffer)
  NXBOX_API_METHOD(SOSetTargets)
  NXBOX_API_METHOD(OMSetRenderTargets)
#undef NXBOX_API_METHOD
};
template <typename T> auto nxbox_api(const T &object, const char *site) {
  auto *raw = nxbox_api_raw(object);
  return NxboxApi<std::remove_pointer_t<decltype(raw)>>{
      raw, nxbox_api_origin() ? nxbox_api_origin() : site};
}
#endif
