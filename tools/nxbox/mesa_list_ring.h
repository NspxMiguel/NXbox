/* SPDX-License-Identifier: MIT
 * Per-command-list recording history. COM private data owns the journal, so
 * destruction and pointer reuse cannot leak or misattribute another list's log.
 * Fixed-size argument snapshots never retain resource/descriptor references.
 */
#pragma once
#include <array>
#include <cstddef>
#include <directx/d3d12sdklayers.h>
#include <memory>
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
  template <typename Out>
  static void describe(Out &out, uintptr_t address,
                       const D3D12_RESOURCE_DESC &d) {
    out.add("res=%p ", (void *)address);
    out.add(
        "{dim=%u format=%u size=%llux%ux%u mips=%u samples=%u:%u flags=0x%x "
        "ALLOW_UNORDERED_ACCESS=%u ALLOW_RENDER_TARGET=%u "
        "ALLOW_DEPTH_STENCIL=%u ALLOW_SIMULTANEOUS_ACCESS=%u} ",
        (unsigned)d.Dimension, (unsigned)d.Format, (unsigned long long)d.Width,
        d.Height, (unsigned)d.DepthOrArraySize, (unsigned)d.MipLevels,
        d.SampleDesc.Count, d.SampleDesc.Quality, (unsigned)d.Flags,
        !!(d.Flags & 4), !!(d.Flags & 1), !!(d.Flags & 2), !!(d.Flags & 32));
  }
  void resource(ID3D12Resource *res) {
    if (!res) {
      add("res=NULL ");
      return;
    }
    describe(*this, (uintptr_t)res, res->GetDesc());
  }
};
// CPU descriptor handles do not encode their heap type. Track live heap
// ranges, with removal tied to COM private-data lifetime, to report actual and
// expected types instead of guessing from OM/Clear's method name.
struct NxboxViewSnapshot {
  uintptr_t resource = 0;
  D3D12_RESOURCE_DESC desc{};
  bool known = false;
};
struct NxboxDescriptorRange {
  UINT64 start;
  UINT increment, count, type;
  std::vector<NxboxViewSnapshot> views;
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
  try {
    {
      std::lock_guard<std::mutex> lock(nxbox_descriptor_mutex());
      nxbox_descriptor_ranges()[owner] = {
          start.ptr,
          dev->GetDescriptorHandleIncrementSize(desc.Type),
          desc.NumDescriptors,
          (UINT)desc.Type,
          {}};
      if ((UINT)desc.Type == 2 || (UINT)desc.Type == 3)
        nxbox_descriptor_ranges()[owner].views.resize(desc.NumDescriptors);
    }
    heap->SetPrivateDataInterface(nxbox_heap_key, owner);
  } catch (const std::bad_alloc &) {
    SetEnvironmentVariableA("NXBOX_D3D12_LIST_ERROR",
                            "descriptor registry allocation failed");
  }
  owner->Release();
}
template <typename Out>
inline void nxbox_cpu_handle(Out &out, D3D12_CPU_DESCRIPTOR_HANDLE handle,
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
      const auto slot = (handle.ptr - range.start) / range.increment;
      if ((handle.ptr - range.start) % range.increment == 0 &&
          slot < range.views.size() && range.views[slot].known) {
        if (range.views[slot].resource)
          NxboxListText::describe(out, range.views[slot].resource,
                                  range.views[slot].desc);
        else
          out.add("res=NULL ");
      } else
        out.add("res=unknown ");
      return;
    }
  }
  out.add("heap_type=unknown res=unknown ");
}
inline void nxbox_view_created(ID3D12Resource *resource,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  NxboxViewSnapshot snapshot;
  snapshot.resource = (uintptr_t)resource;
  snapshot.known = true;
  if (resource)
    snapshot.desc = resource->GetDesc();
  std::lock_guard<std::mutex> lock(nxbox_descriptor_mutex());
  for (auto &entry : nxbox_descriptor_ranges()) {
    auto &r = entry.second;
    if (r.increment && handle.ptr >= r.start &&
        (handle.ptr - r.start) % r.increment == 0) {
      const auto slot = (handle.ptr - r.start) / r.increment;
      if (slot < r.views.size()) {
        r.views[slot] = snapshot;
        return;
      }
    }
  }
}
// A fragment holds deferred printf operations. Arguments are scalar copies or
// pointers to static format/label strings, never pointers into caller arrays.
// Large arrays spill into further fixed records; wraparound is explicit.
struct NxboxListRecord {
  const char *site = nullptr, *name = nullptr;
  uint64_t seq = 0, generation = 0;
  unsigned tid = 0, part = 0, used = 0;
  alignas(std::max_align_t) unsigned char data[464]{};
};
static_assert(sizeof(NxboxListRecord) == 512, "Bound capture memory per list");
struct NxboxListOperation {
  void (*format)(NxboxListText &, const void *);
  unsigned size;
};
struct NxboxListJournal;
struct NxboxListSink {
  NxboxListJournal &journal;
  const char *site, *name;
  uint64_t seq;
  unsigned part = 0;
  NxboxListRecord *entry = nullptr;
  NxboxListRecord &room(unsigned bytes);
  void add(const char *literal) { add("%s", literal); }
  template <typename... A> void add(const char *fmt, A... args) {
    using Values = std::tuple<const char *, A...>;
    static_assert(std::is_trivially_destructible<Values>::value,
                  "Deferred values must not own memory");
    constexpr unsigned alignment = alignof(std::max_align_t);
    constexpr unsigned bytes =
        (sizeof(NxboxListOperation) + sizeof(Values) + alignment - 1) /
        alignment * alignment;
    static_assert(bytes <= sizeof(NxboxListRecord::data),
                  "Operation too large");
    auto &r = room(bytes);
    auto *op = new (r.data + r.used) NxboxListOperation;
    op->size = bytes;
    op->format = [](NxboxListText &out, const void *data) {
      const auto &values = *static_cast<const Values *>(data);
      std::apply([&](const auto &...v) { out.add(v...); }, values);
    };
    new (r.data + r.used + sizeof(NxboxListOperation)) Values(fmt, args...);
    r.used += bytes;
  }
  void resource(ID3D12Resource *res) {
    if (res)
      NxboxListText::describe(*this, (uintptr_t)res, res->GetDesc());
    else
      add("res=NULL ");
  }
};
template <typename Out> struct NxboxListArgsT : Out {
  using Out::add;
  using Out::resource;

  const char *name;
  unsigned index = 0;
  uint64_t numbers[16]{};
  template <typename... A>
  explicit NxboxListArgsT(const char *method, A &&...args)
      : Out{std::forward<A>(args)...}, name(method) {}
  bool is(const char *method) const { return !strcmp(name, method); }
  template <typename T> void number(const T &v) {
    if constexpr (std::is_integral<T>::value || std::is_enum<T>::value)
      if (index < 16)
        numbers[index] = (uint64_t)v;
    ++index;
  }
  uint64_t previous() const {
    return index && index <= 16 ? numbers[index - 1] : 0;
  }
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
  void value(const D3D12_DISCARD_REGION *v) {
    if (!v) {
      add("discard=FULL ");
      return;
    }
    add("first_sub=%u sub_count=%u rect_count=%u ", v->FirstSubresource,
        v->NumSubresources, v->NumRects);
    if (!v->pRects && v->NumRects) {
      add("rects=NULL ");
      return;
    }
    for (UINT i = 0; i < v->NumRects; ++i)
      add("rect[%u]=%ld,%ld:%ld,%ld ", i, (long)v->pRects[i].left,
          (long)v->pRects[i].top, (long)v->pRects[i].right,
          (long)v->pRects[i].bottom);
  }
  void value(const unsigned int *v) {
    if (!v) {
      add("NULL ");
      return;
    }
    if (is("ClearUnorderedAccessViewUint"))
      for (unsigned i = 0; i < 4; ++i)
        add("%u ", v[i]);
    else
      add("%p ", (const void *)v);
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
    if (is("DiscardResource"))
      return "resource region";
    if (is("ClearUnorderedAccessViewUint") ||
        is("ClearUnorderedAccessViewFloat"))
      return "gpu cpu resource values count rects";
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

using NxboxListArgs = NxboxListArgsT<NxboxListText>;

// Conservative static checks, independent of capture availability. No state
// tracker or resource ownership changes; only the first offender is published.
inline bool nxbox_validate_enabled() {
  static const bool enabled = nxbox_env_flag("NXBOX_D3D12_VALIDATE");
  return enabled;
}
inline std::atomic<bool> &nxbox_validation_reported() {
  static std::atomic<bool> reported{false};
  return reported;
}
inline void nxbox_validation_error(const char *name, const char *site,
                                   const char *reason, UINT element = 0) {
  if (nxbox_validation_reported().exchange(true))
    return;
  char text[512];
  snprintf(text, sizeof(text), "call=%s site=%s element=%u reason=%s", name,
           site, element, reason);
  SetEnvironmentVariableA("NXBOX_D3D12_VALIDATE_FIRST", text);
}
// DXGI typeless families. Unknown/new formats are not guessed compatible.
inline unsigned nxbox_format_family(unsigned f) {
  constexpr unsigned ranges[][2] = {
      {1, 4},   {5, 8},   {9, 14},  {15, 18}, {19, 22}, {23, 25}, {27, 32},
      {33, 38}, {39, 43}, {44, 47}, {48, 52}, {53, 59}, {60, 64}, {70, 72},
      {73, 75}, {76, 78}, {79, 81}, {82, 84}, {94, 96}, {97, 99}};
  for (const auto &r : ranges)
    if (f >= r[0] && f <= r[1])
      return r[0];
  if (f == 87 || f == 90 || f == 91)
    return 90;
  if (f == 88 || f == 92 || f == 93)
    return 92;
  return f;
}
inline unsigned nxbox_format_bytes(unsigned f) {
  // Only the documented uncompressed/BC reinterpret-copy pairs need this.
  switch (nxbox_format_family(f)) {
  case 1:
    return 16;
  case 9:
  case 15:
  case 19:
    return 8;
  case 23:
  case 27:
  case 33:
  case 39:
  case 44:
  case 90:
  case 92:
    return 4;
  case 48:
  case 53:
    return 2;
  case 60:
    return 1;
  case 70:
  case 79:
    return 8;
  case 73:
  case 76:
  case 82:
  case 94:
  case 97:
    return 16;
  default:
    return 0;
  }
}
inline bool nxbox_format_bc(unsigned f) {
  return (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
}
inline bool nxbox_formats_compatible(unsigned a, unsigned b) {
  if (a == b || nxbox_format_family(a) == nxbox_format_family(b))
    return true;
  if ((a == 26 && nxbox_format_family(b) == 39) ||
      (b == 26 && nxbox_format_family(a) == 39))
    return true;
  // D3D permits BC blocks to copy to a matching-sized uncompressed texel.
  if (nxbox_format_bc(a) != nxbox_format_bc(b)) {
    const auto bytes = nxbox_format_bytes(a);
    const auto plain = nxbox_format_family(nxbox_format_bc(a) ? b : a);
    return bytes && bytes == nxbox_format_bytes(b) &&
           ((bytes == 8 && (plain == 9 || plain == 15)) ||
            (bytes == 16 && plain == 1));
  }
  return false;
}
struct NxboxValidateExtent {
  uint64_t w = 0, h = 0, d = 0;
  unsigned format = 0;
  bool depth = false;
};
inline NxboxValidateExtent
nxbox_validate_extent(const D3D12_TEXTURE_COPY_LOCATION *loc) {
  if (!loc || !loc->pResource)
    return {};
  const auto desc = loc->pResource->GetDesc();
  if (loc->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    const auto &f = loc->PlacedFootprint.Footprint;
    return {f.Width, f.Height, f.Depth, (unsigned)f.Format, false};
  }
  if (!desc.MipLevels)
    return {};
  const auto mip = loc->SubresourceIndex % desc.MipLevels;
  auto minify = [mip](uint64_t n) {
    return mip < 64 && (n >> mip) ? n >> mip : uint64_t(1);
  };
  auto w = minify(desc.Width), h = minify(desc.Height);
  if (nxbox_format_bc((unsigned)desc.Format)) {
    w = (w + 3) / 4 * 4;
    h = (h + 3) / 4 * 4;
  }
  return {w, h, desc.Dimension == 4 ? minify(desc.DepthOrArraySize) : 1,
          (unsigned)desc.Format, !!(desc.Flags & 2)};
}
inline void nxbox_validate_barriers(const char *name, const char *site,
                                    UINT count,
                                    const D3D12_RESOURCE_BARRIER *barriers) {
  if (!barriers) {
    if (count)
      nxbox_validation_error(name, site, "null-barrier-array");
    return;
  }
  for (UINT i = 0; i < count && !nxbox_validation_reported().load(); ++i) {
    const auto &b = barriers[i];
    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
        b.Type != D3D12_RESOURCE_BARRIER_TYPE_ALIASING &&
        b.Type != D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      nxbox_validation_error(name, site, "invalid-barrier-type", i);
      continue;
    }
    const auto barrier_flags = (unsigned)b.Flags;
    if ((barrier_flags & ~3u) || barrier_flags == 3u ||
        (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barrier_flags)) {
      nxbox_validation_error(name, site, "invalid-barrier-flags", i);
      continue;
    }
    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
      continue;
    if (!b.Transition.pResource) {
      nxbox_validation_error(name, site, "null-transition-resource", i);
      continue;
    }
    const auto flags = b.Transition.pResource->GetDesc().Flags;
    const auto states =
        (unsigned)b.Transition.StateBefore | (unsigned)b.Transition.StateAfter;
    const char *reason = nullptr;
    if ((states & 8) && !(flags & 4))
      reason = "UNORDERED_ACCESS-without-ALLOW_UNORDERED_ACCESS";
    else if ((states & 4) && !(flags & 1))
      reason = "RENDER_TARGET-without-ALLOW_RENDER_TARGET";
    else if ((states & 16) && !(flags & 2))
      reason = "DEPTH_WRITE-without-ALLOW_DEPTH_STENCIL";
    else if (b.Transition.StateBefore == b.Transition.StateAfter)
      reason = "StateBefore-equals-StateAfter";
    if (reason)
      nxbox_validation_error(name, site, reason, i);
  }
}
inline void nxbox_validate_copy(const char *name, const char *site,
                                const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x,
                                UINT y, UINT z,
                                const D3D12_TEXTURE_COPY_LOCATION *src,
                                const D3D12_BOX *box) {
  if (!src || !dst || !src->pResource || !dst->pResource) {
    nxbox_validation_error(name, site, "null-copy-location-or-resource");
    return;
  }
  const auto s = nxbox_validate_extent(src), d = nxbox_validate_extent(dst);
  if (!nxbox_formats_compatible(s.format, d.format)) {
    nxbox_validation_error(name, site, "incompatible-copy-formats");
    return;
  }
  if ((s.depth || d.depth) && (box || x || y || z)) {
    nxbox_validation_error(
        name, site,
        "depth-stencil-copy-requires-full-subresource-and-null-box");
    return;
  }
  const uint64_t l = box ? box->left : 0, t = box ? box->top : 0,
                 f = box ? box->front : 0;
  const uint64_t r = box ? box->right : s.w, b = box ? box->bottom : s.h,
                 back = box ? box->back : s.d;
  // Empty boxes are legal no-ops. Reversed or out-of-bounds coordinates are
  // not.
  if (r < l || b < t || back < f || r > s.w || b > s.h || back > s.d ||
      l > s.w || t > s.h || f > s.d) {
    nxbox_validation_error(name, site, "source-box-out-of-bounds");
    return;
  }
  if (r == l || b == t || back == f)
    return;
  const auto source_block = nxbox_format_bc(s.format) ? 4u : 1u;
  const auto dest_block = nxbox_format_bc(d.format) ? 4u : 1u;
  const auto w = (r - l + source_block - 1) / source_block * dest_block;
  const auto h = (b - t + source_block - 1) / source_block * dest_block;
  if (x > d.w || w > d.w - x || y > d.h || h > d.h - y || z > d.d ||
      back - f > d.d - z)
    nxbox_validation_error(name, site, "destination-box-out-of-bounds");
}
template <typename... A>
inline void nxbox_validate(const char *name, const char *site,
                           const A &...args) {
  if (!nxbox_validate_enabled() || nxbox_validation_reported().load())
    return;
  const auto t = std::tie(args...);
  if constexpr (sizeof...(A) == 2) {
    if constexpr (std::is_convertible<decltype(std::get<0>(t)), UINT>::value &&
                  std::is_convertible<decltype(std::get<1>(t)),
                                      const D3D12_RESOURCE_BARRIER *>::value)
      if (!strcmp(name, "ResourceBarrier"))
        nxbox_validate_barriers(name, site, std::get<0>(t), std::get<1>(t));
  }
  if constexpr (sizeof...(A) == 6) {
    if constexpr (
        std::is_convertible<decltype(std::get<0>(t)),
                            const D3D12_TEXTURE_COPY_LOCATION *>::value &&
        std::is_convertible<decltype(std::get<1>(t)), UINT>::value &&
        std::is_convertible<decltype(std::get<2>(t)), UINT>::value &&
        std::is_convertible<decltype(std::get<3>(t)), UINT>::value &&
        std::is_convertible<decltype(std::get<4>(t)),
                            const D3D12_TEXTURE_COPY_LOCATION *>::value &&
        std::is_convertible<decltype(std::get<5>(t)), const D3D12_BOX *>::value)
      if (!strcmp(name, "CopyTextureRegion"))
        nxbox_validate_copy(name, site, std::get<0>(t), std::get<1>(t),
                            std::get<2>(t), std::get<3>(t), std::get<4>(t),
                            std::get<5>(t));
  }
  if constexpr (sizeof...(A) == 5 || sizeof...(A) == 7) {
    if constexpr (std::is_convertible<decltype(std::get<0>(t)),
                                      ID3D12Resource *>::value &&
                  std::is_convertible<decltype(std::get<1>(t)),
                                      UINT64>::value &&
                  std::is_convertible<decltype(std::get<2>(t)),
                                      ID3D12Resource *>::value &&
                  std::is_convertible<decltype(std::get<3>(t)),
                                      UINT64>::value &&
                  std::is_convertible<decltype(std::get<4>(t)),
                                      UINT64>::value) {
      if (!strcmp(name, "CopyBufferRegion")) {
        ID3D12Resource *dst = std::get<0>(t), *src = std::get<2>(t);
        const UINT64 off_d = std::get<1>(t), off_s = std::get<3>(t),
                     bytes = std::get<4>(t);
        if (!dst || !src) {
          nxbox_validation_error(name, site, "null-copy-buffer");
          return;
        }
        const auto d = dst->GetDesc(), s = src->GetDesc();
        if (d.Dimension != 1 || s.Dimension != 1 || off_d > d.Width ||
            bytes > d.Width - off_d || off_s > s.Width ||
            bytes > s.Width - off_s)
          nxbox_validation_error(name, site,
                                 "buffer-copy-out-of-bounds-or-not-buffer");
      }
    }
  }
}

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
  const unsigned capacity =
      nxbox_env_flag("NXBOX_D3D12_LIST_FULL") ? 4096 : 128;
  std::unique_ptr<NxboxListRecord[]> entries{new (std::nothrow)
                                                 NxboxListRecord[capacity]};
  const bool full = nxbox_env_flag("NXBOX_D3D12_LIST_FULL");
  uint64_t fragments = 0;
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
  template <typename... A>
  void record(const char *site, const char *name, const A &...args) {
    if (!entries)
      return;
    NxboxListArgsT<NxboxListSink> details(name, *this, site, name, next++);
    details.collect(args...);
    // Even argument-free Close must own a record.
    if (!details.entry)
      details.room(0);
  }
};
inline NxboxListRecord &NxboxListSink::room(unsigned bytes) {
  if (!entry || entry->used + bytes > sizeof(entry->data)) {
    entry = &journal.entries[journal.fragments++ % journal.capacity];
    entry->site = site;
    entry->name = name;
    entry->seq = seq;
    entry->generation = journal.generation;
    entry->tid = GetCurrentThreadId();
    entry->part = part++;
    entry->used = 0;
  }
  return *entry;
}
inline NxboxListText nxbox_list_format(const NxboxListRecord &r) {
  NxboxListText out;
  out.add("seq=%llu generation=%llu tid=%lu call=%s site=%s fragment=%u ",
          (unsigned long long)r.seq, (unsigned long long)r.generation,
          (unsigned long)r.tid, r.name, r.site, r.part);
  for (unsigned at = 0; at < r.used;) {
    const auto *op = reinterpret_cast<const NxboxListOperation *>(r.data + at);
    op->format(out, r.data + at + sizeof(NxboxListOperation));
    at += op->size;
  }
  return out;
}
struct NxboxListRef {
  NxboxListJournal *p = nullptr;
  explicit NxboxListRef(ID3D12CommandList *list) {
    UINT size = sizeof(p);
    if (!list || FAILED(list->GetPrivateData(nxbox_list_key, &size, &p)))
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
  if (journal && journal->entries) {
    try {
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
    } catch (const std::bad_alloc &) {
      SetEnvironmentVariableA("NXBOX_D3D12_LIST_ERROR",
                              "creation capture allocation failed");
    }
    journal->Release();
  } else {
    if (journal)
      journal->Release();
    SetEnvironmentVariableA("NXBOX_D3D12_LIST_ERROR",
                            "journal allocation failed");
  }
  list->Release();
}
inline void nxbox_infoqueue_setup(ID3D12Device *dev) {
  if (!dev || !nxbox_debug_enabled())
    return;
  ID3D12InfoQueue *queue = nullptr;
  const HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&queue));
  if (FAILED(hr) || !queue) {
    char text[96];
    snprintf(text, sizeof(text), "hr=0x%08x stage=InfoQueue", (unsigned)hr);
    SetEnvironmentVariableA("NXBOX_D3D12_DEBUG_UNAVAILABLE", text);
    return;
  }
  queue->ClearStorageFilter();
  queue->ClearRetrievalFilter();
  const HRESULT limit = queue->SetMessageCountLimit(4096);
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
inline void nxbox_list_failure_impl(ID3D12CommandList *list, ID3D12Device *dev,
                                    const char *name, const char *site,
                                    HRESULT hr) {
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
    const auto end = journal.p->fragments;
    const auto begin =
        end > journal.p->capacity ? end - journal.p->capacity : 0;
    NxboxListText history;
    history.add(
        "history generation=%llu recorded=%llu retained=%llu "
        "dropped=%llu full=%u fragments=%llu capacity=%u record_bytes=%zu",
        (unsigned long long)journal.p->generation,
        (unsigned long long)journal.p->next, (unsigned long long)(end - begin),
        (unsigned long long)begin, journal.p->full ? 1u : 0u,
        (unsigned long long)end, journal.p->capacity, sizeof(NxboxListRecord));
    out.write("RING", history.text);
    for (auto seq = begin; seq < end; ++seq)
      out.write("RING",
                nxbox_list_format(journal.p->entries[seq % journal.p->capacity])
                    .text);
  } else
    out.write("RING", "journal=unavailable");
  ID3D12InfoQueue *queue = nullptr;
  const HRESULT qi =
      dev ? dev->QueryInterface(IID_PPV_ARGS(&queue)) : E_NOINTERFACE;
  if (SUCCEEDED(qi) && queue) {
    const auto release_queue = [](ID3D12InfoQueue *p) { p->Release(); };
    std::unique_ptr<ID3D12InfoQueue, decltype(release_queue)> queue_ref(
        queue, release_queue);
    const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    NxboxListText stats;
    stats.add(
        "stored=%llu discarded=%llu denied=%llu debug=%u",
        (unsigned long long)count,
        (unsigned long long)queue->GetNumMessagesDiscardedByMessageCountLimit(),
        (unsigned long long)queue->GetNumMessagesDeniedByStorageFilter(),
        nxbox_debug_enabled());
    out.write("INFO", stats.text);
    size_t info_bytes = 0;
    for (UINT64 i = 0; i < count && i < 4096 && info_bytes < 1024 * 1024; ++i) {
      SIZE_T size = 0;
      HRESULT result = queue->GetMessage(i, nullptr, &size);
      if (SUCCEEDED(result) && size >= sizeof(D3D12_MESSAGE) && size <= 65536) {
        std::unique_ptr<unsigned char[]> bytes(
            new (std::nothrow) unsigned char[size]);
        if (!bytes) {
          out.write("INFO", "capture=allocation-failed");
          break;
        }
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.get());
        result = queue->GetMessage(i, message, &size);
        if (SUCCEEDED(result)) {
          NxboxListText text;
          text.add("index=%llu id=%u severity=%u category=%u description=",
                   (unsigned long long)i, (unsigned)message->ID,
                   (unsigned)message->Severity, (unsigned)message->Category);
          if (message->pDescription && message->DescriptionByteLength)
            text.text.append(
                message->pDescription,
                (message->DescriptionByteLength > 65536
                     ? 65536
                     : message->DescriptionByteLength) -
                    (message->DescriptionByteLength <= 65536 &&
                     message->pDescription[message->DescriptionByteLength -
                                           1] == '\0'));
          if (message->DescriptionByteLength > 65536)
            text.add(" description_truncated=1");
          // Keep multiline descriptions in one logical record; preserve bytes
          // except line separators so every physical line has an ID and prefix.
          for (auto &c : text.text)
            if (c == '\n' || c == '\r')
              c = ' ';
          info_bytes += text.text.size();
          out.write("INFO", text.text);
          continue;
        }
      }
      NxboxListText error;
      error.add(
          "index=%llu GetMessage_hr=0x%08x bytes=%llu message_size_limit=65536",
          (unsigned long long)i, (unsigned)result, (unsigned long long)size);
      out.write("INFO", error.text);
    }
    if (count > 4096 || info_bytes >= 1024 * 1024)
      out.write("INFO",
                "capture=truncated count_limit=4096 byte_limit=1048576");
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

inline void nxbox_list_failure(ID3D12CommandList *list, ID3D12Device *dev,
                               const char *name, const char *site, HRESULT hr) {
  try {
    nxbox_list_failure_impl(list, dev, name, site, hr);
  } catch (const std::bad_alloc &) {
    // Keep the terminal stop notification alive even if failure formatting
    // cannot allocate. Never throw into Gallium from a diagnostic capture.
    SetEnvironmentVariableA("NXBOX_D3D12_LIST_ERROR",
                            "failure capture allocation failed");
  }
}
