/* SPDX-License-Identifier: MIT
 * Per-command-list recording history. COM private data owns the journal, so
 * destruction and pointer reuse cannot leak or misattribute another list's log.
 * Argument snapshots own text only, never resource/descriptor references.
 */
#pragma once
#include <array>
#include <directx/d3d12sdklayers.h>
#include <mutex>
#include <new>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

inline bool nxbox_env_flag(const char *name) {
  char value[4]{};
  return GetEnvironmentVariableA(name, value, sizeof(value)) == 1 &&
         value[0] == '1';
}
inline bool nxbox_debug_enabled() {
  static const bool enabled = nxbox_env_flag("NXBOX_D3D12_DEBUG");
  return enabled;
}
struct NxboxListText {
  std::string text;
  void add(const char *literal) { text += literal; }
  template <typename... A> void add(const char *fmt, A... args) {
    const int n = snprintf(nullptr, 0, fmt, args...);
    if (n <= 0)
      return;
    const auto at = text.size();
    text.resize(at + n + 1);
    snprintf(&text[at], n + 1, fmt, args...);
    text.resize(at + n);
  }
  void resource(ID3D12Resource *res) {
    add("res=%p ", (void *)res);
    if (!res)
      return;
    const auto d = res->GetDesc();
    add("{dim=%u format=%u size=%llux%ux%u mips=%u samples=%u:%u flags=0x%x} ",
        (unsigned)d.Dimension, (unsigned)d.Format, (unsigned long long)d.Width,
        d.Height, (unsigned)d.DepthOrArraySize, (unsigned)d.MipLevels,
        d.SampleDesc.Count, d.SampleDesc.Quality, (unsigned)d.Flags);
  }
};
// CPU descriptor handles do not encode their heap type. Track live heap
// ranges, with removal tied to COM private-data lifetime, to report actual and
// expected types instead of guessing from OM/Clear's method name.
struct NxboxDescriptorRange {
  UINT64 start;
  UINT increment, count, type;
};
inline std::mutex &nxbox_descriptor_mutex() {
  static std::mutex value;
  return value;
}
inline std::unordered_map<const void *, NxboxDescriptorRange> &
nxbox_descriptor_ranges() {
  static std::unordered_map<const void *, NxboxDescriptorRange> value;
  return value;
}
inline constexpr GUID nxbox_heap_key{
    0x90a77491,
    0xc506,
    0x4091,
    {0xbb, 0x8c, 0x51, 0xac, 0x4a, 0x2c, 0xfa, 0x17}};
struct NxboxHeapOwner final : IUnknown {
  std::atomic<ULONG> refs{1};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto left = --refs;
    if (!left)
      delete this;
    return left;
  }
  ~NxboxHeapOwner() {
    std::lock_guard<std::mutex> lock(nxbox_descriptor_mutex());
    nxbox_descriptor_ranges().erase(this);
  }
};
inline void nxbox_heap_created(ID3D12Device *dev, ID3D12DescriptorHeap *heap) {
  if (!dev || !heap)
    return;
  auto *owner = new (std::nothrow) NxboxHeapOwner;
  if (!owner)
    return;
  const auto desc = heap->GetDesc();
  const auto start = heap->GetCPUDescriptorHandleForHeapStart();
  {
    std::lock_guard<std::mutex> lock(nxbox_descriptor_mutex());
    nxbox_descriptor_ranges()[owner] = {
        start.ptr, dev->GetDescriptorHandleIncrementSize(desc.Type),
        desc.NumDescriptors, (UINT)desc.Type};
  }
  heap->SetPrivateDataInterface(nxbox_heap_key, owner);
  owner->Release();
}
inline void nxbox_cpu_handle(NxboxListText &out,
                             D3D12_CPU_DESCRIPTOR_HANDLE handle,
                             const char *expected) {
  std::lock_guard<std::mutex> lock(nxbox_descriptor_mutex());
  out.add("cpu=%llu expected=%s ", (unsigned long long)handle.ptr, expected);
  for (const auto &entry : nxbox_descriptor_ranges()) {
    const auto &range = entry.second;
    if (range.increment && handle.ptr >= range.start &&
        (handle.ptr - range.start) / range.increment < range.count) {
      out.add("heap_type=%u heap_start=%llu aligned=%u ", range.type,
              (unsigned long long)range.start,
              (unsigned)((handle.ptr - range.start) % range.increment == 0));
      return;
    }
  }
  out.add("heap_type=unknown ");
}
struct NxboxListArgs : NxboxListText {
  const char *name;
  unsigned index = 0;
  uint64_t numbers[16]{};
  explicit NxboxListArgs(const char *method) : name(method) {}
  bool is(const char *method) const { return !strcmp(name, method); }
  template <typename T> void number(const T &v) {
    if constexpr (std::is_integral<T>::value || std::is_enum<T>::value)
      if (index < 16)
        numbers[index] = (uint64_t)v;
    ++index;
  }
  uint64_t previous() const { return index ? numbers[index - 1] : 0; }
  template <typename T> void value(const T &v) {
    if constexpr (std::is_integral<T>::value && std::is_signed<T>::value)
      add("%lld ", (long long)v);
    else if constexpr (std::is_integral<T>::value || std::is_enum<T>::value)
      add("%llu ", (unsigned long long)v);
    else if constexpr (std::is_floating_point<T>::value)
      add("%g ", (double)v);
    else if constexpr (std::is_pointer<T>::value)
      add("%p ", (const void *)v);
    else if constexpr (std::is_same<T, std::nullptr_t>::value)
      add("NULL ");
    else
      add("object ");
  }
  void value(ID3D12Resource *v) { resource(v); }
  void value(D3D12_CPU_DESCRIPTOR_HANDLE v) {
    nxbox_cpu_handle(*this, v,
                     is("ClearRenderTargetView")   ? "RTV"
                     : is("ClearDepthStencilView") ? "DSV"
                                                   : "unknown");
  }
  void value(D3D12_GPU_DESCRIPTOR_HANDLE v) {
    add("gpu=%llu ", (unsigned long long)v.ptr);
  }
  void value(const D3D12_RESOURCE_BARRIER *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < previous(); ++i) {
      const auto &b = v[i];
      add("barrier[%llu]={type=%u flags=0x%x ", (unsigned long long)i,
          (unsigned)b.Type, (unsigned)b.Flags);
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        resource(b.Transition.pResource);
        add("sub=%u before=0x%x after=0x%x ", b.Transition.Subresource,
            (unsigned)b.Transition.StateBefore,
            (unsigned)b.Transition.StateAfter);
      } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
        resource(b.UAV.pResource);
      else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        add("before=");
        resource(b.Aliasing.pResourceBefore);
        add("after=");
        resource(b.Aliasing.pResourceAfter);
      }
      add("} ");
    }
  }
  void value(const D3D12_TEXTURE_COPY_LOCATION *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    resource(v->pResource);
    add("type=%u ", (unsigned)v->Type);
    if (v->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
      add("sub=%u ", v->SubresourceIndex);
    else {
      const auto &p = v->PlacedFootprint;
      add("footprint={offset=%llu format=%u size=%ux%ux%u pitch=%u} ",
          (unsigned long long)p.Offset, (unsigned)p.Footprint.Format,
          p.Footprint.Width, p.Footprint.Height, p.Footprint.Depth,
          p.Footprint.RowPitch);
    }
  }
  void value(const D3D12_BOX *v) {
    if (v)
      add("box=%u,%u,%u:%u,%u,%u ", v->left, v->top, v->front, v->right,
          v->bottom, v->back);
    else
      add("box=FULL ");
  }
  void value(const D3D12_RECT *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < previous(); ++i)
      add("rect[%llu]=%ld,%ld:%ld,%ld ", (unsigned long long)i, (long)v[i].left,
          (long)v[i].top, (long)v[i].right, (long)v[i].bottom);
  }
  void value(const D3D12_VIEWPORT *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < previous(); ++i)
      add("viewport[%llu]=%g,%g:%gx%g depth=%g:%g ", (unsigned long long)i,
          v[i].TopLeftX, v[i].TopLeftY, v[i].Width, v[i].Height, v[i].MinDepth,
          v[i].MaxDepth);
  }
  void value(const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < numbers[0]; ++i)
      add("write[%llu]={address=%llu value=0x%08x} ", (unsigned long long)i,
          (unsigned long long)v[i].Dest, v[i].Value);
  }
  void value(const D3D12_WRITEBUFFERIMMEDIATE_MODE *v) {
    if (!v) {
      add("DEFAULT ");
      return;
    }
    for (uint64_t i = 0; i < numbers[0]; ++i)
      add("mode[%llu]=%u ", (unsigned long long)i, (unsigned)v[i]);
  }
  void value(const D3D12_INDEX_BUFFER_VIEW *v) {
    if (v)
      add("ibv={address=%llu bytes=%u format=%u} ",
          (unsigned long long)v->BufferLocation, v->SizeInBytes,
          (unsigned)v->Format);
    else
      add("ibv=NULL ");
  }
  void value(const D3D12_VERTEX_BUFFER_VIEW *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < previous(); ++i)
      add("vbv[%llu]={address=%llu bytes=%u stride=%u} ", (unsigned long long)i,
          (unsigned long long)v[i].BufferLocation, v[i].SizeInBytes,
          v[i].StrideInBytes);
  }
  void value(const D3D12_STREAM_OUTPUT_BUFFER_VIEW *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < previous(); ++i)
      add("so[%llu]={address=%llu bytes=%llu filled=%llu} ",
          (unsigned long long)i, (unsigned long long)v[i].BufferLocation,
          (unsigned long long)v[i].SizeInBytes,
          (unsigned long long)v[i].BufferFilledSizeLocation);
  }
  void value(const D3D12_CPU_DESCRIPTOR_HANDLE *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    const auto count = is("OMSetRenderTargets") && index == 1
                           ? (numbers[2] ? (numbers[0] ? 1 : 0) : numbers[0])
                           : 1;
    for (uint64_t i = 0; i < count; ++i) {
      add("handle[%llu]=", (unsigned long long)i);
      nxbox_cpu_handle(*this, v[i], index == 1 ? "RTV" : "DSV");
    }
  }
  void value(ID3D12DescriptorHeap *const *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (uint64_t i = 0; i < previous(); ++i) {
      add("heap[%llu]=%p ", (unsigned long long)i, (void *)v[i]);
      if (v[i]) {
        const auto d = v[i]->GetDesc();
        add("{type=%u flags=0x%x count=%u node=%u} ", (unsigned)d.Type,
            (unsigned)d.Flags, d.NumDescriptors, d.NodeMask);
      }
    }
  }
  void value(const float *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    for (unsigned i = 0; i < 4; ++i)
      add("%g ", v[i]);
  }
  void value(const void *v) {
    add("%p ", v);
    if (v && (is("SetGraphicsRoot32BitConstants") ||
              is("SetComputeRoot32BitConstants"))) {
      const auto *words = static_cast<const uint32_t *>(v);
      for (uint64_t i = 0; i < previous(); ++i)
        add("0x%08x ", words[i]);
    }
  }
  // Normalize mutable pointers and C arrays; otherwise a generic template
  // would win over the const overload and silently print only an address.
  template <typename T> void value(T *v) {
    if constexpr (std::is_same<T, ID3D12Resource>::value)
      resource(v);
    else if constexpr (std::is_base_of<IUnknown, T>::value)
      add("%p ", (void *)v);
    else if constexpr (!std::is_const<T>::value)
      value(static_cast<const T *>(v));
    else
      add("%p ", (const void *)v);
  }
  template <typename T, size_t N> void value(const T (&v)[N]) { value(v + 0); }
  const char *labels() const {
    if (is("ResourceBarrier"))
      return "count barriers";
    if (is("CopyTextureRegion"))
      return "dst x y z src box";
    if (is("CopyBufferRegion"))
      return "dst dst_offset src src_offset bytes dst_size src_size";
    if (is("CopyResource"))
      return "dst src";
    if (is("ResolveSubresource"))
      return "dst dst_sub src src_sub format";
    if (is("BeginQuery") || is("EndQuery"))
      return "heap type index";
    if (is("ResolveQueryData"))
      return "heap type start count dst offset";
    if (is("DrawInstanced"))
      return "vertices instances first_vertex first_instance";
    if (is("DrawIndexedInstanced"))
      return "indices instances first_index base_vertex first_instance";
    if (is("Dispatch"))
      return "x y z";
    if (is("ExecuteIndirect"))
      return "signature count args offset counter counter_offset";
    if (is("ClearRenderTargetView"))
      return "rtv color count rects";
    if (is("ClearDepthStencilView"))
      return "dsv flags depth stencil count rects";
    if (is("OMSetRenderTargets"))
      return "count rtvs contiguous dsv";
    if (is("SetPipelineState"))
      return "pso";
    if (is("SetGraphicsRootSignature") || is("SetComputeRootSignature"))
      return "signature";
    if (strstr(name, "RootDescriptorTable"))
      return "root_parameter handle";
    if (strstr(name, "Root32BitConstants"))
      return "root_parameter count values offset";
    if (strstr(name, "Root32BitConstant"))
      return "root_parameter value offset";
    if (is("Reset"))
      return "allocator initial_pso";
    return "";
  }
  template <typename... A> void collect(const A &...args) {
    (number(args), ...);
    index = 0;
    const char *label = labels();
    auto append = [&](const auto &arg) {
      if (*label) {
        const auto *end = strchr(label, ' ');
        add("%.*s=", (int)(end ? end - label : strlen(label)), label);
        label = end ? end + 1 : label + strlen(label);
      } else
        add("arg%u=", index);
      if constexpr (std::is_pointer<std::decay_t<decltype(arg)>>::value) {
        if (index == 2 && (is("SetGraphicsRoot32BitConstants") ||
                           is("SetComputeRoot32BitConstants")))
          value(static_cast<const void *>(arg));
        else if constexpr (std::is_array<
                               std::remove_reference_t<decltype(arg)>>::value)
          value(arg + 0);
        else
          value(arg);
      } else
        value(arg);
      ++index;
    };
    (append(args), ...);
  }
};

// A unique process-independent key; no Mesa ABI or command-list vtable changes.
inline constexpr GUID nxbox_list_key{
    0x8c09570e,
    0x18a9,
    0x48d1,
    {0xb4, 0x3b, 0xad, 0x6e, 0x51, 0xc0, 0x73, 0x97}};
struct NxboxListJournal final : IUnknown {
  std::atomic<ULONG> refs{1};
  std::mutex mutex;
  std::string creation;
  std::array<std::string, 128> entries;
  uint64_t next = 0, generation = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto left = --refs;
    if (!left)
      delete this;
    return left;
  }
  void record(const char *site, const char *name, const std::string &args) {
    NxboxListText line;
    line.add("seq=%llu generation=%llu tid=%lu call=%s site=%s ",
             (unsigned long long)next, (unsigned long long)generation,
             (unsigned long)GetCurrentThreadId(), name, site);
    entries[next++ % entries.size()] = line.text + args;
  }
};
struct NxboxListRef {
  NxboxListJournal *p = nullptr;
  explicit NxboxListRef(ID3D12CommandList *list) {
    UINT size = sizeof(p);
    if (FAILED(list->GetPrivateData(nxbox_list_key, &size, &p)))
      p = nullptr;
  }
  ~NxboxListRef() {
    if (p)
      p->Release();
  }
  NxboxListRef(const NxboxListRef &) = delete;
};
inline void nxbox_list_created(IUnknown *object, const char *site,
                               const char *args) {
  if (!object)
    return;
  ID3D12CommandList *list = nullptr;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&list))))
    return;
  auto *journal = new (std::nothrow) NxboxListJournal;
  if (journal) {
    NxboxListText creation;
    creation.add("list=%p type=%u site=%s ms=%llu tid=%lu %s", (void *)list,
                 (unsigned)list->GetType(), site,
                 (unsigned long long)GetTickCount64(),
                 (unsigned long)GetCurrentThreadId(), args);
    journal->creation = std::move(creation.text);
    const HRESULT hr = list->SetPrivateDataInterface(nxbox_list_key, journal);
    if (FAILED(hr))
      SetEnvironmentVariableA("NXBOX_D3D12_LIST_ERROR",
                              "SetPrivateDataInterface failed");
    journal->Release();
  } else
    SetEnvironmentVariableA("NXBOX_D3D12_LIST_ERROR",
                            "journal allocation failed");
  list->Release();
}
inline void nxbox_infoqueue_setup(ID3D12Device *dev) {
  if (!nxbox_debug_enabled())
    return;
  ID3D12InfoQueue *queue = nullptr;
  const HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&queue));
  if (FAILED(hr)) {
    char text[96];
    snprintf(text, sizeof(text), "hr=0x%08x stage=InfoQueue", (unsigned)hr);
    SetEnvironmentVariableA("NXBOX_D3D12_DEBUG_UNAVAILABLE", text);
    return;
  }
  queue->ClearStorageFilter();
  queue->ClearRetrievalFilter();
  const HRESULT limit = queue->SetMessageCountLimit(UINT64_MAX);
  char status[128];
  snprintf(status, sizeof(status), "enabled=1 infoqueue=1 limit_hr=0x%08x",
           (unsigned)limit);
  SetEnvironmentVariableA("NXBOX_D3D12_DEBUG_STATUS", status);
  queue->Release();
}
// Immutable, uniquely numbered captures. Chunk long commands/descriptions
// without truncation; each physical line carries the capture and part index.
struct NxboxListPublisher {
  unsigned capture, ring_parts = 0, info_parts = 0;
  void write(const char *kind, const std::string &text) {
    auto &part = !strcmp(kind, "RING") ? ring_parts : info_parts;
    for (size_t offset = 0; offset < text.size(); offset += 3500) {
      char key[96], prefix[96];
      snprintf(key, sizeof(key), "NXBOX_D3D12_LIST_%u_%s_%u", capture, kind,
               part);
      snprintf(prefix, sizeof(prefix), "capture=%u part=%u offset=%zu ",
               capture, part++, offset);
      const auto value = std::string(prefix) + text.substr(offset, 3500);
      SetEnvironmentVariableA(key, value.c_str());
    }
  }
};
inline void nxbox_list_failure(ID3D12CommandList *list, ID3D12Device *dev,
                               const char *name, const char *site, HRESULT hr) {
  static std::mutex mutex;
  static unsigned captures = 0;
  static const unsigned limit =
      nxbox_env_flag("NXBOX_D3D12_DUMP_EVERY_CLOSE") ? 5 : 1;
  std::lock_guard<std::mutex> lock(mutex);
  if (captures >= limit)
    return;
  NxboxListPublisher out{captures};
  NxboxListText header;
  const auto &api = nxbox_api_ring();
  header.add("failure call=%s site=%s list=%p hr=0x%08x removed=0x%08x "
             "custom_created=%llu custom_bytes=%llu custom_unknown=%llu",
             name, site, (void *)list, (unsigned)hr,
             (unsigned)(dev ? dev->GetDeviceRemovedReason() : S_OK),
             (unsigned long long)api.custom_created.load(),
             (unsigned long long)api.custom_bytes.load(),
             (unsigned long long)api.custom_unknown.load());
  out.write("RING", header.text);
  NxboxListRef journal(list);
  if (journal.p) {
    std::lock_guard<std::mutex> guard(journal.p->mutex);
    out.write("RING", "creation " + journal.p->creation);
    const auto end = journal.p->next;
    const auto begin = end > 128 ? end - 128 : 0;
    for (auto seq = begin; seq < end; ++seq)
      out.write("RING", journal.p->entries[seq % 128]);
  } else
    out.write("RING", "journal=unavailable");
  ID3D12InfoQueue *queue = nullptr;
  const HRESULT qi =
      dev ? dev->QueryInterface(IID_PPV_ARGS(&queue)) : E_NOINTERFACE;
  if (SUCCEEDED(qi)) {
    const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    NxboxListText stats;
    stats.add(
        "stored=%llu discarded=%llu denied=%llu debug=%u",
        (unsigned long long)count,
        (unsigned long long)queue->GetNumMessagesDiscardedByMessageCountLimit(),
        (unsigned long long)queue->GetNumMessagesDeniedByStorageFilter(),
        nxbox_debug_enabled());
    out.write("INFO", stats.text);
    for (UINT64 i = 0; i < count; ++i) {
      SIZE_T size = 0;
      HRESULT result = queue->GetMessage(i, nullptr, &size);
      if (SUCCEEDED(result) && size >= sizeof(D3D12_MESSAGE)) {
        std::vector<unsigned char> bytes(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
        result = queue->GetMessage(i, message, &size);
        if (SUCCEEDED(result)) {
          NxboxListText text;
          text.add("index=%llu id=%u severity=%u category=%u description=",
                   (unsigned long long)i, (unsigned)message->ID,
                   (unsigned)message->Severity, (unsigned)message->Category);
          if (message->pDescription && message->DescriptionByteLength)
            text.text.append(
                message->pDescription,
                message->DescriptionByteLength -
                    (message->pDescription[message->DescriptionByteLength -
                                           1] == '\0'));
          // Keep multiline descriptions in one logical record; preserve bytes
          // except line separators so every physical line has an ID and prefix.
          for (auto &c : text.text)
            if (c == '\n' || c == '\r')
              c = ' ';
          out.write("INFO", text.text);
          continue;
        }
      }
      NxboxListText error;
      error.add("index=%llu GetMessage_hr=0x%08x bytes=%llu",
                (unsigned long long)i, (unsigned)result,
                (unsigned long long)size);
      out.write("INFO", error.text);
    }
    queue->Release();
  } else {
    NxboxListText error;
    error.add("unavailable hr=0x%08x debug=%u", (unsigned)qi,
              nxbox_debug_enabled());
    out.write("INFO", error.text);
  }
  char key[80], manifest[128];
  snprintf(key, sizeof(key), "NXBOX_D3D12_LIST_CAPTURE_%u", captures);
  snprintf(manifest, sizeof(manifest), "capture=%u ring_parts=%u info_parts=%u",
           captures, out.ring_parts, out.info_parts);
  SetEnvironmentVariableA(key, manifest);
  snprintf(manifest, sizeof(manifest), "%u", ++captures);
  SetEnvironmentVariableA("NXBOX_D3D12_LIST_CAPTURES", manifest);
}
