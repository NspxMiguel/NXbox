/* SPDX-License-Identifier: MIT
 * Opt-in CPU journal. No COM references are retained and no commands are
 * changed. Include after d3d12_context.h; the context owns this lazily
 * allocated POD ring.
 */
#pragma once
#include <atomic>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

inline bool nxbox_sync_batch_enabled() {
  static const bool enabled = [] {
    char value[4] = {};
    return GetEnvironmentVariableA("NXBOX_SYNC_BATCH", value, sizeof(value)) ==
               1 &&
           value[0] == '1';
  }();
  return enabled;
}

struct NxboxBatchJournal {
  uint64_t next;
  ID3D12PipelineState *pso;
  char entries[64][480];
};

inline void nxbox_journal_reset(NxboxBatchJournal *&journal) {
  if (!nxbox_sync_batch_enabled())
    return;
  if (!journal)
    journal = new (std::nothrow) NxboxBatchJournal{};
  if (journal) {
    journal->next = 0;
    journal->pso = nullptr; // CommandList::Reset starts with a null PSO.
  }
}

inline void nxbox_journal_add(NxboxBatchJournal *journal, const char *list,
                              const char *format, ...) {
  if (!journal)
    return;
  const uint64_t sequence = journal->next++;
  char *text = journal->entries[sequence % 64];
  int prefix =
      snprintf(text, 480, "%llu %s ", (unsigned long long)sequence, list);
  va_list args;
  va_start(args, format);
  int size = vsnprintf(text + prefix, 480 - prefix, format, args);
  va_end(args);
  if (size < 0 || size >= 480 - prefix)
    memcpy(text + 475, "...", 4);
}

/* A temporary wrapper preserves argument evaluation and unbraced if/else
 * semantics. The null-journal path only forwards the original call.
 */
struct NxboxJournalCommands {
  NxboxBatchJournal *journal;
  ID3D12GraphicsCommandList *commands;
  const char *list;

  void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
    if (journal) {
      for (UINT i = 0; i < count; ++i) {
        const auto &b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
          nxbox_journal_add(
              journal, list,
              "ResourceBarrier res=%p sub=%u before=%x after=%x flags=%x",
              (void *)b.Transition.pResource, b.Transition.Subresource,
              (unsigned)b.Transition.StateBefore,
              (unsigned)b.Transition.StateAfter, (unsigned)b.Flags);
        else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
          nxbox_journal_add(journal, list,
                            "ResourceBarrier UAV res=%p flags=%x",
                            (void *)b.UAV.pResource, (unsigned)b.Flags);
        else
          nxbox_journal_add(journal, list,
                            "ResourceBarrier alias before=%p after=%p flags=%x",
                            (void *)b.Aliasing.pResourceBefore,
                            (void *)b.Aliasing.pResourceAfter,
                            (unsigned)b.Flags);
      }
    }
    commands->ResourceBarrier(count, barriers);
  }
  void CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset,
                        ID3D12Resource *src, UINT64 src_offset, UINT64 bytes) {
    if (journal)
      nxbox_journal_add(
          journal, list, "CopyBufferRegion dst=%p+%llu src=%p+%llu bytes=%llu",
          (void *)dst, (unsigned long long)dst_offset, (void *)src,
          (unsigned long long)src_offset, (unsigned long long)bytes);
    commands->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes);
  }
  static void location(char *text, size_t size,
                       const D3D12_TEXTURE_COPY_LOCATION *loc) {
    if (!loc || !loc->pResource) {
      snprintf(text, size, "NULL-location-or-resource");
      return;
    }
    if (loc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      const auto desc = loc->pResource->GetDesc();
      snprintf(text, size, "%p/sub=%u/fmt=%u/size=%llux%ux%u/mips=%u/ms=%u",
               (void *)loc->pResource, loc->SubresourceIndex,
               (unsigned)desc.Format, (unsigned long long)desc.Width,
               desc.Height, (unsigned)desc.DepthOrArraySize,
               (unsigned)desc.MipLevels, desc.SampleDesc.Count);
    } else {
      const auto &p = loc->PlacedFootprint;
      snprintf(text, size, "%p/offset=%llu/fmt=%u/size=%ux%ux%u/pitch=%u",
               (void *)loc->pResource, (unsigned long long)p.Offset,
               (unsigned)p.Footprint.Format, p.Footprint.Width,
               p.Footprint.Height, p.Footprint.Depth, p.Footprint.RowPitch);
    }
  }
  void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y,
                         UINT z, const D3D12_TEXTURE_COPY_LOCATION *src,
                         const D3D12_BOX *box) {
    if (journal) {
      char d[144], s[144], b[96];
      location(d, sizeof(d), dst);
      location(s, sizeof(s), src);
      if (box)
        snprintf(b, sizeof(b), "%u,%u,%u:%u,%u,%u", box->left, box->top,
                 box->front, box->right, box->bottom, box->back);
      else
        strcpy(b, "FULL");
      nxbox_journal_add(journal, list,
                        "CopyTextureRegion dst=%s xyz=%u,%u,%u src=%s box=%s",
                        d, x, y, z, s, b);
    }
    commands->CopyTextureRegion(dst, x, y, z, src, box);
  }
  void CopyResource(ID3D12Resource *dst, ID3D12Resource *src) {
    if (journal)
      nxbox_journal_add(journal, list, "CopyResource dst=%p src=%p",
                        (void *)dst, (void *)src);
    commands->CopyResource(dst, src);
  }
  void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view,
                             const FLOAT *color, UINT count,
                             const D3D12_RECT *rects) {
    if (journal)
      nxbox_journal_add(
          journal, list,
          "ClearRenderTargetView view=%llu rgba=%g,%g,%g,%g rects=%u "
          "first=%ld,%ld,%ld,%ld",
          (unsigned long long)view.ptr, color[0], color[1], color[2], color[3],
          count, count ? (long)rects[0].left : 0L,
          count ? (long)rects[0].top : 0L, count ? (long)rects[0].right : 0L,
          count ? (long)rects[0].bottom : 0L);
    commands->ClearRenderTargetView(view, color, count, rects);
  }
  void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view,
                             D3D12_CLEAR_FLAGS flags, FLOAT depth,
                             UINT8 stencil, UINT count,
                             const D3D12_RECT *rects) {
    if (journal)
      nxbox_journal_add(
          journal, list,
          "ClearDepthStencilView view=%llu flags=%x depth=%g stencil=%u "
          "rects=%u first=%ld,%ld,%ld,%ld",
          (unsigned long long)view.ptr, (unsigned)flags, depth,
          (unsigned)stencil, count, count ? (long)rects[0].left : 0L,
          count ? (long)rects[0].top : 0L, count ? (long)rects[0].right : 0L,
          count ? (long)rects[0].bottom : 0L);
    commands->ClearDepthStencilView(view, flags, depth, stencil, count, rects);
  }
  void ResolveSubresource(ID3D12Resource *dst, UINT dst_sub,
                          ID3D12Resource *src, UINT src_sub,
                          DXGI_FORMAT format) {
    if (journal)
      nxbox_journal_add(
          journal, list, "ResolveSubresource dst=%p/%u src=%p/%u fmt=%u",
          (void *)dst, dst_sub, (void *)src, src_sub, (unsigned)format);
    commands->ResolveSubresource(dst, dst_sub, src, src_sub, format);
  }
  void SetPipelineState(ID3D12PipelineState *pso) {
    if (journal) {
      journal->pso = pso;
      nxbox_journal_add(journal, list, "SetPipelineState pso=%p", (void *)pso);
    }
    commands->SetPipelineState(pso);
  }
  void SetGraphicsRootSignature(ID3D12RootSignature *signature) {
    if (journal)
      nxbox_journal_add(journal, list, "SetGraphicsRootSignature rs=%p",
                        (void *)signature);
    commands->SetGraphicsRootSignature(signature);
  }
  void SetComputeRootSignature(ID3D12RootSignature *signature) {
    if (journal)
      nxbox_journal_add(journal, list, "SetComputeRootSignature rs=%p",
                        (void *)signature);
    commands->SetComputeRootSignature(signature);
  }
  void DrawInstanced(UINT vertices, UINT instances, UINT first,
                     UINT first_instance) {
    if (journal)
      nxbox_journal_add(journal, list,
                        "DrawInstanced pso=%p vertices=%u instances=%u "
                        "first=%u first_instance=%u",
                        (void *)journal->pso, vertices, instances, first,
                        first_instance);
    commands->DrawInstanced(vertices, instances, first, first_instance);
  }
  void DrawIndexedInstanced(UINT indices, UINT instances, UINT first, INT base,
                            UINT first_instance) {
    if (journal)
      nxbox_journal_add(journal, list,
                        "DrawIndexedInstanced pso=%p indices=%u instances=%u "
                        "first=%u base=%d first_instance=%u",
                        (void *)journal->pso, indices, instances, first, base,
                        first_instance);
    commands->DrawIndexedInstanced(indices, instances, first, base,
                                   first_instance);
  }
  void Dispatch(UINT x, UINT y, UINT z) {
    if (journal)
      nxbox_journal_add(journal, list, "Dispatch pso=%p xyz=%u,%u,%u",
                        (void *)journal->pso, x, y, z);
    commands->Dispatch(x, y, z);
  }
  void ExecuteIndirect(ID3D12CommandSignature *signature, UINT count,
                       ID3D12Resource *args, UINT64 offset,
                       ID3D12Resource *counter, UINT64 counter_offset) {
    if (journal)
      nxbox_journal_add(
          journal, list,
          "ExecuteIndirect pso=%p sig=%p count=%u args=%p+%llu counter=%p+%llu",
          (void *)journal->pso, (void *)signature, count, (void *)args,
          (unsigned long long)offset, (void *)counter,
          (unsigned long long)counter_offset);
    commands->ExecuteIndirect(signature, count, args, offset, counter,
                              counter_offset);
  }
};

inline NxboxJournalCommands
nxbox_journal_commands(NxboxBatchJournal *journal,
                       ID3D12GraphicsCommandList *commands,
                       const char *list = "main") {
  return {journal, commands, list};
}

inline std::atomic<bool> &nxbox_sync_stopped() {
  static std::atomic<bool> stopped{false};
  return stopped;
}

/* Called with screen->submit_mutex held, including the entire GPU wait. A
 * separate latch from DRED is essential: a PSO worker or Signal may have
 * observed loss first. Publish the manifest LAST so readers cannot mistake a
 * partial journal for a complete one.
 */
inline void nxbox_sync_publish(const NxboxBatchJournal *journal,
                               const void *ctx, unsigned batch, uint64_t submit,
                               uint64_t fence, HRESULT removed,
                               bool fixup_executed) {
  static std::atomic<bool> captured{false};
  if (captured.exchange(true))
    return;
  const uint64_t next = journal ? journal->next : 0;
  const uint64_t first = next > 64 ? next - 64 : 0;
  char text[4096], name[64];
  unsigned part = 0;
  size_t used = 0;
  for (uint64_t i = first; i < next; ++i) {
    const char *entry = journal->entries[i % 64];
    const size_t length = strlen(entry);
    if (used + length + 1 >= sizeof(text)) {
      text[used] = 0;
      snprintf(name, sizeof(name), "NXBOX_D3D12_BATCH_JOURNAL_%u", part++);
      SetEnvironmentVariableA(name, text);
      used = 0;
    }
    memcpy(text + used, entry, length);
    used += length;
    text[used++] = '\n';
  }
  if (used) {
    text[used] = 0;
    snprintf(name, sizeof(name), "NXBOX_D3D12_BATCH_JOURNAL_%u", part++);
    SetEnvironmentVariableA(name, text);
  }
  snprintf(text, sizeof(text),
           "NXBOX_D3D12_FIRST_BAD_BATCH id=%llu ctx=%p batch=%u removed=0x%08x "
           "fence=%llu "
           "parts=%u entries=%llu dropped=%llu journal=%s order=recorded "
           "gpu_order=fixup,main fixup_executed=%u",
           (unsigned long long)submit, ctx, batch, (unsigned)removed,
           (unsigned long long)fence, part, (unsigned long long)(next - first),
           (unsigned long long)first, journal ? "ok" : "allocation-failed",
           fixup_executed ? 1u : 0u);
  SetEnvironmentVariableA("NXBOX_D3D12_FIRST_BAD_BATCH", text);
  /* The current frontend already reads this key; full journal parts use the
   * keys above. */
  SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", text);
}

/* Poll the actual target, not d3d12_fence_finish's removal-as-completion
 * result. UINT64_MAX is the D3D12 removal sentinel and must never become
 * fence_ok.
 */
inline HRESULT nxbox_sync_wait(ID3D12Device *dev, ID3D12Fence *fence,
                               UINT64 target) {
  for (;;) {
    const UINT64 completed = fence->GetCompletedValue();
    const HRESULT removed = dev->GetDeviceRemovedReason();
    if (FAILED(removed))
      return removed;
    if (completed != UINT64_MAX && completed >= target)
      return dev->GetDeviceRemovedReason();
    Sleep(1);
  }
}
