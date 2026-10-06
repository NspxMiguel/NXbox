// SPDX-License-Identifier: GPL-3.0-or-later
// Deliberately narrow host COM model; SDK syntax is checked separately.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using HRESULT = int32_t;
using UINT = unsigned;
using ULONG = unsigned;
using UINT64 = uint64_t;
using SIZE_T = size_t;
using REFIID = unsigned;
using D3D12_DESCRIPTOR_HEAP_TYPE = unsigned;
constexpr HRESULT S_OK = 0, E_INVALIDARG = -22, E_NOINTERFACE = -2,
                  E_POINTER = -3;
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)
#define STDMETHODCALLTYPE
#define __uuidof(type) 1u
#define IID_PPV_ARGS(pp) 1u, reinterpret_cast<void **>(pp)
struct GUID {
  unsigned a;
  unsigned short b, c;
  unsigned char d[8];
};
struct IUnknown {
  virtual HRESULT QueryInterface(REFIID, void **out) {
    *out = this;
    AddRef();
    return S_OK;
  }
  virtual ULONG AddRef() { return 2; }
  virtual ULONG Release() { return 1; }
};
std::mutex env_mutex;
std::map<std::string, std::string> env;
std::vector<std::string> published;
unsigned GetEnvironmentVariableA(const char *name, char *out, unsigned size) {
  std::lock_guard<std::mutex> lock(env_mutex);
  auto it = env.find(name);
  if (it == env.end())
    return 0;
  unsigned n = it->second.size();
  if (n < size)
    memcpy(out, it->second.c_str(), n + 1);
  return n;
}
void SetEnvironmentVariableA(const char *name, const char *value) {
  std::lock_guard<std::mutex> lock(env_mutex);
  env[name] = value;
  published.emplace_back(name);
}
uint64_t GetTickCount64() { return 1; }
unsigned GetCurrentThreadId() { return 1; }
void Sleep(unsigned) { std::this_thread::yield(); }
constexpr unsigned D3D12_HEAP_TYPE_CUSTOM = 4;
struct D3D12_RESOURCE_DESC {
  unsigned Alignment = 0, Layout = 0;
  uint64_t Width = 4096;
  unsigned Height = 1, DepthOrArraySize = 1, Format = 28, Flags = 4,
           Dimension = 2, MipLevels = 1;
  struct {
    unsigned Count = 1, Quality = 0;
  } SampleDesc;
};
struct D3D12_RESOURCE_DESC1 : D3D12_RESOURCE_DESC {};
struct D3D12_HEAP_PROPERTIES {
  unsigned Type = 2, CPUPageProperty = 0, MemoryPoolPreference = 0;
};
struct D3D12_HEAP_DESC {
  uint64_t SizeInBytes = 8192;
  unsigned Flags = 0;
  D3D12_HEAP_PROPERTIES Properties;
};
struct D3D12_DESCRIPTOR_HEAP_DESC {
  unsigned NumDescriptors = 512, Type = 1, Flags = 1, NodeMask = 0;
};
struct D3D12_GPU_DESCRIPTOR_HANDLE {
  uint64_t ptr;
};
struct D3D12_CPU_DESCRIPTOR_HANDLE {
  uint64_t ptr;
};
struct ID3D12Resource : IUnknown {
  D3D12_RESOURCE_DESC desc;
  auto GetDesc() { return desc; }
};
struct ID3D12DescriptorHeap final : IUnknown {
  D3D12_DESCRIPTOR_HEAP_DESC desc;
  IUnknown *data = nullptr;
  uint64_t start = 100;
  auto GetDesc() { return desc; }
  D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() {
    return {start};
  }
  HRESULT SetPrivateDataInterface(const GUID &, IUnknown *v) {
    if (v)
      v->AddRef();
    if (data)
      data->Release();
    data = v;
    return S_OK;
  }
  ~ID3D12DescriptorHeap() {
    if (data)
      data->Release();
  }
};
constexpr unsigned D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0,
                   D3D12_RESOURCE_BARRIER_TYPE_UAV = 1,
                   D3D12_RESOURCE_BARRIER_TYPE_ALIASING = 2;
struct D3D12_RESOURCE_BARRIER {
  unsigned Type = 0, Flags = 0;
  struct {
    ID3D12Resource *pResource = nullptr;
    unsigned Subresource = 0, StateBefore = 0, StateAfter = 0;
  } Transition;
  struct {
    ID3D12Resource *pResource = nullptr;
  } UAV;
  struct {
    ID3D12Resource *pResourceBefore = nullptr, *pResourceAfter = nullptr;
  } Aliasing;
};
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0;
struct D3D12_TEXTURE_COPY_LOCATION {
  ID3D12Resource *pResource = nullptr;
  unsigned Type = 0, SubresourceIndex = 0;
  struct {
    uint64_t Offset = 0;
    struct {
      unsigned Format = 0, Width = 1, Height = 1, Depth = 1, RowPitch = 256;
    } Footprint;
  } PlacedFootprint;
};
struct D3D12_BOX {
  unsigned left, top, front, right, bottom, back;
};
struct D3D12_RECT {
  long left, top, right, bottom;
};
struct D3D12_VIEWPORT {
  float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
};
struct D3D12_WRITEBUFFERIMMEDIATE_PARAMETER {
  uint64_t Dest;
  unsigned Value;
};
enum D3D12_WRITEBUFFERIMMEDIATE_MODE {
  D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT
};
struct D3D12_INDEX_BUFFER_VIEW {
  uint64_t BufferLocation;
  unsigned SizeInBytes, Format;
};
struct D3D12_VERTEX_BUFFER_VIEW {
  uint64_t BufferLocation;
  unsigned SizeInBytes, StrideInBytes;
};
struct D3D12_STREAM_OUTPUT_BUFFER_VIEW {
  uint64_t BufferLocation, SizeInBytes, BufferFilledSizeLocation;
};
struct D3D12_MESSAGE {
  unsigned ID, Severity, Category;
  const char *pDescription;
  size_t DescriptionByteLength;
};
struct ID3D12InfoQueue : IUnknown {
  std::vector<std::string> messages;
  unsigned setups = 0;
  void ClearStorageFilter() { ++setups; }
  void ClearRetrievalFilter() { ++setups; }
  HRESULT SetMessageCountLimit(uint64_t v) {
    assert(v == UINT64_MAX);
    ++setups;
    return 0;
  }
  uint64_t GetNumStoredMessagesAllowedByRetrievalFilter() {
    return messages.size();
  }
  uint64_t GetNumMessagesDiscardedByMessageCountLimit() { return 0; }
  uint64_t GetNumMessagesDeniedByStorageFilter() { return 0; }
  HRESULT GetMessage(uint64_t index, D3D12_MESSAGE *out, size_t *size) {
    *size = sizeof(D3D12_MESSAGE);
    if (out)
      *out = {unsigned(index), 2, 3, messages[index].c_str(),
              messages[index].size() + 1};
    return 0;
  }
};
struct ID3D12GraphicsCommandList;
struct ID3D12Device : IUnknown {
  ID3D12InfoQueue *queue = nullptr;
  unsigned calls = 0;
  unsigned GetDescriptorHandleIncrementSize(unsigned) { return 16; }
  HRESULT GetDeviceRemovedReason() { return 0; }
  HRESULT QueryInterface(REFIID, void **out) override {
    *out = queue;
    return queue ? S_OK : E_NOINTERFACE;
  }
  struct AllocationInfo {
    uint64_t SizeInBytes;
  };
  AllocationInfo GetResourceAllocationInfo(unsigned, unsigned,
                                           const D3D12_RESOURCE_DESC *) {
    return {65536};
  }
  HRESULT CreateCommandList(unsigned, unsigned, IUnknown *, IUnknown *, REFIID,
                            void **);
  HRESULT CreateCommandList1(unsigned, unsigned, unsigned, REFIID, void **);
};
struct ID3D12DeviceChild : IUnknown {
  ID3D12Device *dev = nullptr;
  HRESULT GetDevice(REFIID, void **out) {
    *out = dev;
    return dev ? S_OK : E_NOINTERFACE;
  }
};
struct ID3D12CommandList : ID3D12DeviceChild {
  IUnknown *data = nullptr;
  HRESULT GetPrivateData(const GUID &, UINT *size, void *out) {
    assert(*size == sizeof(data));
    if (!data)
      return E_NOINTERFACE;
    data->AddRef();
    *static_cast<IUnknown **>(out) = data;
    return S_OK;
  }
  HRESULT SetPrivateDataInterface(const GUID &, IUnknown *v) {
    if (v)
      v->AddRef();
    if (data)
      data->Release();
    data = v;
    return S_OK;
  }
  unsigned GetType() { return 0; }
  ~ID3D12CommandList() {
    if (data)
      data->Release();
  }
};
struct ID3D12CommandAllocator : ID3D12DeviceChild {};
struct ID3D12CommandQueue : ID3D12DeviceChild {};
struct ID3D12GraphicsCommandList final : ID3D12CommandList {
  HRESULT close_hr = 0, reset_hr = 0;
  unsigned draws = 0, closes = 0, resets = 0, barriers = 0;
  HRESULT Close() {
    ++closes;
    return close_hr;
  }
  HRESULT Reset(ID3D12CommandAllocator *, IUnknown *) {
    ++resets;
    return reset_hr;
  }
  void DrawInstanced(unsigned, unsigned, unsigned, unsigned) { ++draws; }
  void ResourceBarrier(unsigned, const D3D12_RESOURCE_BARRIER *) { ++barriers; }
};
HRESULT ID3D12Device::CreateCommandList(unsigned, unsigned, IUnknown *,
                                        IUnknown *, REFIID, void **out) {
  auto *list = new ID3D12GraphicsCommandList;
  list->dev = this;
  *out = list;
  ++calls;
  return S_OK;
}
HRESULT ID3D12Device::CreateCommandList1(unsigned node, unsigned type, unsigned,
                                         REFIID iid, void **out) {
  return CreateCommandList(node, type, nullptr, nullptr, iid, out);
}
void nxbox_dred_capture(ID3D12Device *, HRESULT, const char *, const char *) {}
