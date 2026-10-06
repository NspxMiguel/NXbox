/* SPDX-License-Identifier: MIT */
#pragma once
#include "nxbox_dred.h"

/* SetEventOnCompletion(target, NULL) blocks inside D3D12 with no opportunity
 * to notice a lost device. Poll instead, including for nonblocking queries.
 * A lost query is retired with a zero result by the caller, never read back.
 */
static bool nxbox_query_ready(ID3D12Device *dev, ID3D12Fence *fence,
                              UINT64 target, bool wait,
                              bool (*stopped)() = nullptr) {
  for (;;) {
    if (stopped && stopped())
      return true; // Caller retires the unsubmitted query without mapping it.
    const HRESULT removed = dev->GetDeviceRemovedReason();
    const UINT64 completed = fence->GetCompletedValue();
    if (FAILED(removed) || completed == UINT64_MAX) {
      nxbox_dred_capture(dev,
                         FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED,
                         "query-wait");
      return true;
    }
    // UINT64_MAX means this query was never submitted, not a waitable target.
    if (target == UINT64_MAX)
      return false;
    if (completed >= target)
      return true;
    if (!wait)
      return false;
    Sleep(1);
  }
}
