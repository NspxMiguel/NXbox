/* SPDX-License-Identifier: MIT */
#pragma once

#include "nxbox_dred.h"

/* Events and the cached d3d12_fence::signaled bit are notifications, not proof
 * that a batch's allocator, descriptors and suballocated upload BOs are idle.
 * This reuse guard also protects NXBOX_SYNC_BATCH=0 submissions. A zero timeout
 * is a nonblocking query and UINT64_MAX is removal, never successful GPU work.
 */
static bool nxbox_batch_wait(ID3D12Device *dev, ID3D12Fence *fence, UINT64 target,
                             uint64_t timeout_ns) {
   const ULONGLONG started = GetTickCount64();
   const uint64_t timeout_ms = timeout_ns / 1000000 + (timeout_ns % 1000000 != 0);
   for (;;) {
      const UINT64 completed = fence->GetCompletedValue();
      const HRESULT removed = dev->GetDeviceRemovedReason();
      if (FAILED(removed)) {
         nxbox_dred_capture(dev, removed, "batch-reuse");
         return true; // Dead-device cleanup is safe, allocator Reset is skipped.
      }
      if (completed != UINT64_MAX && completed >= target)
         return true;
      if (completed == UINT64_MAX)
         return false; // Do not recycle on an unconfirmed removal sentinel.
      if (!timeout_ns ||
          (timeout_ns != OS_TIMEOUT_INFINITE && GetTickCount64() - started >= timeout_ms))
         return false;
      Sleep(1);
   }
}
