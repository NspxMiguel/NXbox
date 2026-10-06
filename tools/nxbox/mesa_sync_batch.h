/* SPDX-License-Identifier: MIT
 * Synchronous submission and CPU journal, enabled unless NXBOX_SYNC_BATCH=0.
 * No COM references are retained by the journal. Include after d3d12_context.h;
 * the context owns this lazily allocated journal (sync diagnostics only).
 */
#pragma once
#include <atomic>
#include <mutex>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

inline bool nxbox_sync_batch_enabled() {
  static const bool enabled = [] {
    char value[4] = {};
    return !(GetEnvironmentVariableA("NXBOX_SYNC_BATCH", value,
                                     sizeof(value)) == 1 &&
             value[0] == '0');
  }();
  return enabled;
}

struct NxboxBatchJournal {
  uint64_t next, fixup_next;
  ID3D12PipelineState *pso;
  ID3D12RootSignature *graphics_root, *compute_root;
  char targets[256];
  std::vector<std::string> entries, fixup_entries;
};

inline void nxbox_journal_reset(NxboxBatchJournal *&journal) {
  if (!nxbox_sync_batch_enabled())
    return;
  if (!journal)
    journal = new (std::nothrow) NxboxBatchJournal{};
  if (journal) {
    journal->next = journal->fixup_next = 0;
    journal->entries.clear();
    journal->fixup_entries.clear();
    journal->pso = nullptr; // CommandList::Reset starts with a null PSO.
    journal->graphics_root = journal->compute_root = nullptr;
    journal->targets[0] = 0;
  }
}

inline void nxbox_journal_add(NxboxBatchJournal *journal, const char *list,
                              const char *format, ...) {
  if (!journal)
    return;
  const bool fixup = strcmp(list, "fixup") == 0;
  const uint64_t sequence = fixup ? journal->fixup_next++ : journal->next++;
  char text[480];
  int prefix =
      snprintf(text, 480, "%llu %s ", (unsigned long long)sequence, list);
  va_list args;
  va_start(args, format);
  int size = vsnprintf(text + prefix, 480 - prefix, format, args);
  va_end(args);
  if (size < 0 || size >= 480 - prefix)
    memcpy(text + 475, "...", 4);
  (fixup ? journal->fixup_entries : journal->entries).emplace_back(text);
}

/* A temporary wrapper preserves argument evaluation and unbraced if/else
 * semantics. The null-journal path only forwards the original call.
 */
struct NxboxJournalCommands {
  NxboxBatchJournal *journal;
  ID3D12GraphicsCommandList *commands;
  const char *list;

  static void resource(char *text, size_t size, ID3D12Resource *res) {
    if (!res) {
      snprintf(text, size, "NULL");
      return;
    }
    const auto d = res->GetDesc();
    snprintf(text, size,
             "%p/dim=%u/fmt=%u/size=%llux%ux%u/mips=%u/ms=%u:%u/flags=%x",
             (void *)res, (unsigned)d.Dimension, (unsigned)d.Format,
             (unsigned long long)d.Width, d.Height,
             (unsigned)d.DepthOrArraySize, (unsigned)d.MipLevels,
             d.SampleDesc.Count, d.SampleDesc.Quality, (unsigned)d.Flags);
  }
  void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
    if (journal) {
      for (UINT i = 0; i < count; ++i) {
        const auto &b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
          char desc[192];
          resource(desc, sizeof(desc), b.Transition.pResource);
          nxbox_journal_add(
              journal, list,
              "ResourceBarrier res=%s sub=%u before=%x after=%x flags=%x", desc,
              b.Transition.Subresource, (unsigned)b.Transition.StateBefore,
              (unsigned)b.Transition.StateAfter, (unsigned)b.Flags);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
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
    if (journal) {
      char d[192], s[192];
      resource(d, sizeof(d), dst);
      resource(s, sizeof(s), src);
      nxbox_journal_add(
          journal, list, "CopyBufferRegion dst=%s+%llu src=%s+%llu bytes=%llu",
          d, (unsigned long long)dst_offset, s, (unsigned long long)src_offset,
          (unsigned long long)bytes);
    }
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
    if (journal) {
      char d[192], s[192];
      resource(d, sizeof(d), dst);
      resource(s, sizeof(s), src);
      nxbox_journal_add(journal, list, "CopyResource dst=%s src=%s", d, s);
    }
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
    if (journal) {
      journal->graphics_root = signature;
      nxbox_journal_add(journal, list, "SetGraphicsRootSignature rs=%p",
                        (void *)signature);
    }
    commands->SetGraphicsRootSignature(signature);
  }
  void SetComputeRootSignature(ID3D12RootSignature *signature) {
    if (journal) {
      journal->compute_root = signature;
      nxbox_journal_add(journal, list, "SetComputeRootSignature rs=%p",
                        (void *)signature);
    }
    commands->SetComputeRootSignature(signature);
  }
  void DrawInstanced(UINT vertices, UINT instances, UINT first,
                     UINT first_instance) {
    if (journal)
      nxbox_journal_add(
          journal, list,
          "DrawInstanced pso=%p rs=%p %s vertices=%u instances=%u "
          "first=%u first_instance=%u",
          (void *)journal->pso, (void *)journal->graphics_root,
          journal->targets, vertices, instances, first, first_instance);
    commands->DrawInstanced(vertices, instances, first, first_instance);
  }
  void DrawIndexedInstanced(UINT indices, UINT instances, UINT first, INT base,
                            UINT first_instance) {
    if (journal)
      nxbox_journal_add(
          journal, list,
          "DrawIndexedInstanced pso=%p rs=%p %s indices=%u instances=%u "
          "first=%u base=%d first_instance=%u",
          (void *)journal->pso, (void *)journal->graphics_root,
          journal->targets, indices, instances, first, base, first_instance);
    commands->DrawIndexedInstanced(indices, instances, first, base,
                                   first_instance);
  }
  void Dispatch(UINT x, UINT y, UINT z) {
    if (journal)
      nxbox_journal_add(journal, list, "Dispatch pso=%p rs=%p xyz=%u,%u,%u",
                        (void *)journal->pso, (void *)journal->compute_root, x,
                        y, z);
    commands->Dispatch(x, y, z);
  }
  void ExecuteBundle(ID3D12GraphicsCommandList *bundle) {
    nxbox_journal_add(journal, list, "ExecuteBundle bundle=%p pso=%p",
                      (void *)bundle, journal ? (void *)journal->pso : nullptr);
    commands->ExecuteBundle(bundle);
  }
  void ExecuteIndirect(ID3D12CommandSignature *signature, UINT count,
                       ID3D12Resource *args, UINT64 offset,
                       ID3D12Resource *counter, UINT64 counter_offset) {
    if (journal)
      nxbox_journal_add(journal, list,
                        "ExecuteIndirect pso=%p gfx_rs=%p cs_rs=%p %s sig=%p "
                        "count=%u args=%p+%llu counter=%p+%llu",
                        (void *)journal->pso, (void *)journal->graphics_root,
                        (void *)journal->compute_root, journal->targets,
                        (void *)signature, count, (void *)args,
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

/* Called with nxbox_submission_mutex held, from any removal observer.
 * Publish the manifest after all chunks so readers cannot mistake a partial
 * journal for a complete one. The DRED exit notification follows both.
 */
inline void nxbox_sync_publish(const NxboxBatchJournal *journal,
                               const void *ctx, unsigned batch, uint64_t submit,
                               uint64_t fence, HRESULT removed,
                               bool fixup_executed, bool completed,
                               bool previous = false) {
  const uint64_t fixup = journal && fixup_executed ? journal->fixup_next : 0;
  const uint64_t next = journal ? fixup + journal->next : 0;
  const uint64_t first = 0;
  const char *key = previous ? "NXBOX_D3D12_PREVIOUS_GOOD_BATCH"
                             : "NXBOX_D3D12_FIRST_BAD_BATCH";
  const char *prefix = previous ? "NXBOX_D3D12_BATCH_JOURNAL_PREVIOUS"
                                : "NXBOX_D3D12_BATCH_JOURNAL";
  char text[4096], name[64];
  unsigned part = 0;
  size_t used = 0;
  for (uint64_t i = first; i < next; ++i) {
    const char *entry = i < fixup ? journal->fixup_entries[i].c_str()
                                  : journal->entries[i - fixup].c_str();
    const size_t length = strlen(entry);
    if (used + length + 1 >= sizeof(text)) {
      text[used] = 0;
      snprintf(name, sizeof(name), "%s_%u", prefix, part++);
      SetEnvironmentVariableA(name, text);
      used = 0;
    }
    memcpy(text + used, entry, length);
    used += length;
    text[used++] = '\n';
  }
  if (used) {
    text[used] = 0;
    snprintf(name, sizeof(name), "%s_%u", prefix, part++);
    SetEnvironmentVariableA(name, text);
  }
  snprintf(text, sizeof(text),
           "%s id=%llu ctx=%p batch=%u removed=0x%08x "
           "fence=%llu "
           "parts=%u entries=%llu dropped=%llu journal=%s order=execution "
           "gpu_order=fixup,main fixup_executed=%u completed=%u attribution=%s",
           key, (unsigned long long)submit, ctx, batch, (unsigned)removed,
           (unsigned long long)fence, part, (unsigned long long)(next - first),
           (unsigned long long)first, journal ? "ok" : "allocation-failed",
           fixup_executed ? 1u : 0u, completed ? 1u : 0u,
           previous ? "checked-good-submit"
                    : (completed ? "delayed-after-checked-submit"
                                 : "in-flight-submit"));
  SetEnvironmentVariableA(key, text);
  /* The current frontend already reads this key; full journal parts use the
   * keys above. */
  if (!previous)
    SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", text);
}

/* Immutable current and preceding checked-good submission snapshots per device.
 * DRED can run on another worker, after a context reset, or before the
 * submitting thread returns from Signal. Never call DRED while holding this
 * mutex (DRED takes it to publish before loss).
 */
struct NxboxSubmission {
  NxboxBatchJournal journal{};
  const void *ctx = nullptr;
  unsigned batch = 0;
  uint64_t submit = 0, fence = 0;
  bool allocated = false, fixup = false, completed = false;
};
struct NxboxSubmittedJournal : NxboxSubmission {
  NxboxSubmission previous;
  bool captured = false;
};
inline std::mutex &nxbox_submission_mutex() {
  static std::mutex mutex;
  return mutex;
}
inline std::unordered_map<ID3D12Device *, NxboxSubmittedJournal> &
nxbox_submissions() {
  static std::unordered_map<ID3D12Device *, NxboxSubmittedJournal> submissions;
  return submissions;
}
inline void nxbox_sync_submit(ID3D12Device *dev,
                              const NxboxBatchJournal *journal, const void *ctx,
                              unsigned batch, uint64_t submit, uint64_t fence,
                              bool fixup) {
  if (!nxbox_sync_batch_enabled())
    return;
  std::lock_guard<std::mutex> lock(nxbox_submission_mutex());
  auto &s = nxbox_submissions()[dev];
  if (s.captured)
    return;
  if (s.completed)
    s.previous = static_cast<const NxboxSubmission &>(s);
  if (journal)
    s.journal = *journal;
  s.allocated = journal != nullptr;
  s.ctx = ctx;
  s.batch = batch;
  s.submit = submit;
  s.fence = fence;
  s.fixup = fixup;
  s.completed = false;
}
inline void nxbox_sync_completed(ID3D12Device *dev) {
  if (!nxbox_sync_batch_enabled())
    return;
  std::lock_guard<std::mutex> lock(nxbox_submission_mutex());
  auto it = nxbox_submissions().find(dev);
  if (it != nxbox_submissions().end() && !it->second.captured)
    it->second.completed = true;
}
inline void nxbox_sync_capture(ID3D12Device *dev, HRESULT removed) {
  if (!nxbox_sync_batch_enabled() || removed == S_OK)
    return;
  std::lock_guard<std::mutex> lock(nxbox_submission_mutex());
  auto it = nxbox_submissions().find(dev);
  if (it == nxbox_submissions().end())
    return; // No Execute to attribute.
  auto &s = it->second;
  if (s.captured)
    return;
  s.captured = true;
  const auto &p = s.previous;
  if (p.completed)
    nxbox_sync_publish(p.allocated ? &p.journal : nullptr, p.ctx, p.batch,
                       p.submit, p.fence, S_OK, p.fixup, true, true);
  nxbox_sync_publish(s.allocated ? &s.journal : nullptr, s.ctx, s.batch,
                     s.submit, s.fence, removed, s.fixup, s.completed);
}
inline void nxbox_sync_forget(ID3D12Device *dev) {
  std::lock_guard<std::mutex> lock(nxbox_submission_mutex());
  nxbox_submissions().erase(dev);
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
    if (removed != S_OK)
      return removed;
    if (completed == UINT64_MAX)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (completed >= target)
      return dev->GetDeviceRemovedReason();
    Sleep(1);
  }
}
