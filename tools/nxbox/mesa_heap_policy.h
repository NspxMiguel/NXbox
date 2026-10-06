/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdio>

// Mesa queries ARCHITECTURE during screen initialization and fails if
// unavailable. NOT_AVAILABLE/L0 is a documented UMA mapping, but GPU-only
// allocations need no custom mapping. Keep CUSTOM only where Mesa requires CPU
// mapping in COMMON.
inline D3D12_HEAP_PROPERTIES
nxbox_heap_properties(const D3D12_FEATURE_DATA_ARCHITECTURE &architecture,
                      D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  properties.CreationNodeMask = properties.VisibleNodeMask = 1;
  if (type == D3D12_HEAP_TYPE_UPLOAD || type == D3D12_HEAP_TYPE_READBACK) {
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    properties.CPUPageProperty =
        type == D3D12_HEAP_TYPE_READBACK ||
                (architecture.UMA && architecture.CacheCoherentUMA)
            ? D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
            : D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
  }
  return properties;
}
inline void
nxbox_report_heap_policy(const D3D12_FEATURE_DATA_ARCHITECTURE &architecture,
                         bool not_resident) {
  char text[256];
  snprintf(text, sizeof(text),
           "uma=%u coherent=%u gpu=DEFAULT mapped=CUSTOM_L0 "
           "create_not_resident=%u create_not_zeroed=0",
           (unsigned)architecture.UMA, (unsigned)architecture.CacheCoherentUMA,
           not_resident ? 1u : 0u);
  SetEnvironmentVariableA("NXBOX_D3D12_HEAP_POLICY", text);
}
