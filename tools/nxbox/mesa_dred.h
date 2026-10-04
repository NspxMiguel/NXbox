/* SPDX-License-Identifier: MIT
 * Diagnostic support copied into the pinned Mesa tree by patch_dred().
 * Define NXBOX_DRED_IMPLEMENTATION in d3d12_screen.cpp only.
 */
#pragma once

void nxbox_dred_capture(ID3D12Device *dev, HRESULT removed, const char *where,
                        const char *batch_text = nullptr);
void nxbox_dred_submit(ID3D12Device *dev, const void *ctx, unsigned batch,
                       unsigned long long submit, unsigned long long fence);

void nxbox_dred_publish_batch(ID3D12Device *dev, const char *text);

#ifdef NXBOX_DRED_IMPLEMENTATION
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

namespace {
struct NxboxDredText {
   char text[501] = {};
   void
   add(const char *format, ...) {
      size_t used = strlen(text);
      if (used == sizeof(text) - 1)
         return;
      va_list args;
      va_start(args, format);
      int n = vsnprintf(text + used, sizeof(text) - used, format, args);
      va_end(args);
      if (n < 0 || (size_t)n >= sizeof(text) - used) {
         text[sizeof(text) - 2] = '~';
         text[sizeof(text) - 1] = 0;
      }
   }
};
struct NxboxDredState {
   bool captured = false;
   unsigned long long next = 0;
   char submissions[4][96] = {};
};
std::mutex nxbox_dred_mutex;
std::unordered_map<ID3D12Device *, NxboxDredState> nxbox_dred_states;

/* Avoid locale-dependent wide formatting and keep every diagnostic on one line. */
static void
nxbox_dred_name(char (&out)[41], const char *narrow, const wchar_t *wide) {
   unsigned i = 0;
   for (; i < sizeof(out) - 1; ++i) {
      unsigned c = narrow ? (unsigned char)narrow[i] : wide ? (unsigned)wide[i] : 0;
      if (!c)
         break;
      out[i] = c >= 32 && c < 127 ? (char)c : '?';
   }
   out[i] = 0;
   if (!i)
      strcpy(out, "-");
}

#ifndef _GAMING_XBOX
static void
nxbox_dred_enable(util_dl_library *module, ID3D12DeviceFactory *factory) {
   NxboxDredText status;
#if defined(__ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__)
   typedef HRESULT(WINAPI * GetDebugInterface)(REFIID, void **);
   auto get = (GetDebugInterface)util_dl_get_proc_address(module, "D3D12GetDebugInterface");
   const char *stage = factory ? "FactoryConfiguration" : "GetDebugInterface";
   auto settings_interface = [&](REFIID iid, void **out) -> HRESULT {
      /* A factory has isolated configuration; do not fall back to a different runtime. */
      if (factory)
         return factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, iid, out);
      return get ? get(iid, out) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
   };
   HRESULT hr1 = E_NOINTERFACE;
#if defined(__ID3D12DeviceRemovedExtendedDataSettings1_INTERFACE_DEFINED__)
   ID3D12DeviceRemovedExtendedDataSettings1 *settings1 = nullptr;
   hr1 = settings_interface(IID_PPV_ARGS(&settings1));
   if (SUCCEEDED(hr1)) {
      settings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings1->Release();
      status.add("enabled settings=1 breadcrumbs=on pagefault=on contexts=on via=%s", stage);
   } else
#endif
   {
      ID3D12DeviceRemovedExtendedDataSettings *settings = nullptr;
      HRESULT hr = settings_interface(IID_PPV_ARGS(&settings));
      if (SUCCEEDED(hr)) {
         settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
         settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
         settings->Release();
         status.add("enabled settings=0 breadcrumbs=on pagefault=on contexts=unavailable "
                    "hr1=0x%08lx via=%s",
                    (unsigned long)hr1, stage);
      } else {
         status.add("unavailable stage=%s hr=0x%08lx hr1=0x%08lx", stage, (unsigned long)hr,
                    (unsigned long)hr1);
      }
   }
#else
   status.add("unavailable stage=SDK.Settings hr=0x%08lx", (unsigned long)E_NOINTERFACE);
#endif
   SetEnvironmentVariableA("NXBOX_D3D12_DRED2", "pending device removal");
   SetEnvironmentVariableA("NXBOX_D3D12_DRED", status.text);
}
#endif

#if defined(__ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__)
static const char *
nxbox_dred_op(D3D12_AUTO_BREADCRUMB_OP op) {
#define NXBOX_DRED_OP(name)                                                                        \
   case D3D12_AUTO_BREADCRUMB_OP_##name:                                                           \
      return #name
   switch (op) {
      NXBOX_DRED_OP(SETMARKER);
      NXBOX_DRED_OP(BEGINEVENT);
      NXBOX_DRED_OP(ENDEVENT);
      NXBOX_DRED_OP(DRAWINSTANCED);
      NXBOX_DRED_OP(DRAWINDEXEDINSTANCED);
      NXBOX_DRED_OP(EXECUTEINDIRECT);
      NXBOX_DRED_OP(DISPATCH);
      NXBOX_DRED_OP(COPYBUFFERREGION);
      NXBOX_DRED_OP(COPYTEXTUREREGION);
      NXBOX_DRED_OP(COPYRESOURCE);
      NXBOX_DRED_OP(COPYTILES);
      NXBOX_DRED_OP(RESOLVESUBRESOURCE);
      NXBOX_DRED_OP(CLEARRENDERTARGETVIEW);
      NXBOX_DRED_OP(CLEARUNORDEREDACCESSVIEW);
      NXBOX_DRED_OP(CLEARDEPTHSTENCILVIEW);
      NXBOX_DRED_OP(RESOURCEBARRIER);
      NXBOX_DRED_OP(EXECUTEBUNDLE);
      NXBOX_DRED_OP(RESOLVEQUERYDATA);
      NXBOX_DRED_OP(PRESENT);
   default:
      return nullptr;
   }
#undef NXBOX_DRED_OP
}

static void
nxbox_dred_contexts(NxboxDredText &, const D3D12_AUTO_BREADCRUMB_NODE *, unsigned) {}
#if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
static void
nxbox_dred_contexts(NxboxDredText &out, const D3D12_AUTO_BREADCRUMB_NODE1 *node, unsigned id) {
   if (!node->pBreadcrumbContexts)
      return;
   UINT last = *node->pLastBreadcrumbValue;
   /* Include the closest preceding context, even if the marker is older than three ops. */
   const D3D12_DRED_BREADCRUMB_CONTEXT *nearest = nullptr;
   for (UINT i = 0; i < node->BreadcrumbContextsCount && i < 65536; ++i) {
      const auto &context = node->pBreadcrumbContexts[i];
      if (context.BreadcrumbIndex <= last &&
          (!nearest || context.BreadcrumbIndex > nearest->BreadcrumbIndex))
         nearest = &context;
   }
   if (nearest) {
      char name[41];
      nxbox_dred_name(name, nullptr, nearest->pContextString);
      out.add(" ctx%u[%u]=%s", id, nearest->BreadcrumbIndex, name);
   }
}
#endif

template <typename Node>
static void
nxbox_dred_breadcrumbs(NxboxDredText &out, NxboxDredText &contexts, const Node *node) {
   unsigned shown = 0, visited = 0, missing = 0;
   for (; node && visited < 4096 && shown < 2; node = node->pNext, ++visited) {
      if (!node->pLastBreadcrumbValue) {
         ++missing;
         continue;
      }
      UINT last = *node->pLastBreadcrumbValue;
      if (last >= node->BreadcrumbCount)
         continue;
      char list[41], queue[41];
      nxbox_dred_name(list, node->pCommandListDebugNameA, node->pCommandListDebugNameW);
      nxbox_dred_name(queue, node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
      out.add(" n%u cl=%s q=%s count=%u last=%u ops=", shown, list, queue, node->BreadcrumbCount,
              last);
      /* last is a completed-op COUNT: history[last] is the first unfinished op.
       * History is a 65536-entry ring; overwritten entries cannot identify an op. */
      UINT oldest = node->BreadcrumbCount > 65536 ? node->BreadcrumbCount - 65536 : 0;
      UINT previous = last > 3 ? 3 : last;
      for (UINT back = 0; back <= previous; ++back) {
         UINT i = last - back;
         out.add("%s%u:", back ? "," : "", i);
         if (!node->pCommandHistory)
            out.add("missing");
         else if (i < oldest)
            out.add("overwritten");
         else {
            auto op = node->pCommandHistory[i % 65536];
            const char *name = nxbox_dred_op(op);
            if (name)
               out.add("%s", name);
            else
               out.add("OP(%u)", (unsigned)op);
         }
      }
      nxbox_dred_contexts(contexts, node, shown);
      ++shown;
   }
   out.add(" nodes=%u scan=%u missing=%u more=%u", shown, visited, missing, node ? 1 : 0);
}

template <typename Node>
static void
nxbox_dred_allocations(NxboxDredText &out, const char *label, const Node *node) {
   out.add(" %s=", label);
   if (!node)
      out.add("none");
   for (unsigned i = 0; node && i < 2; ++i, node = node->pNext) {
      char name[41];
      nxbox_dred_name(name, node->ObjectNameA, node->ObjectNameW);
      const char *type = node->AllocationType == D3D12_DRED_ALLOCATION_TYPE_RESOURCE ? "RESOURCE"
                         : node->AllocationType == D3D12_DRED_ALLOCATION_TYPE_HEAP   ? "HEAP"
                                                                                     : "TYPE";
      out.add("%s%s/%s(%u)", i ? "," : "", name, type, (unsigned)node->AllocationType);
   }
   if (node)
      out.add(",more");
}

template <typename Output>
static void
nxbox_dred_pagefault(NxboxDredText &out, HRESULT hr, const Output &page) {
   out.add("page_hr=0x%08lx", (unsigned long)hr);
   if (SUCCEEDED(hr)) {
      out.add(" va=0x%llx", (unsigned long long)page.PageFaultVA);
      nxbox_dred_allocations(out, "existing", page.pHeadExistingAllocationNode);
      nxbox_dred_allocations(out, "freed", page.pHeadRecentFreedAllocationNode);
   }
}
#endif

static void
nxbox_dred_forget(ID3D12Device *dev) {
   std::lock_guard<std::mutex> lock(nxbox_dred_mutex);
   nxbox_dred_states.erase(dev);
}
} // namespace

void
nxbox_dred_submit(ID3D12Device *dev, const void *ctx, unsigned batch, unsigned long long submit,
                  unsigned long long fence) {
   std::lock_guard<std::mutex> lock(nxbox_dred_mutex);
   auto &state = nxbox_dred_states[dev];
   if (state.captured)
      return;
   auto &entry = state.submissions[state.next++ % 4];
   snprintf(entry, sizeof(entry), "ctx=%p b=%u s=%llu f=%llu", ctx, batch, submit, fence);
}

void
nxbox_dred_publish_batch(ID3D12Device *dev, const char *text) {
   std::lock_guard<std::mutex> lock(nxbox_dred_mutex);
   /* Another worker can detect removal after this batch observed S_OK. */
   if (!nxbox_dred_states[dev].captured)
      SetEnvironmentVariableA("NXBOX_D3D12_BATCH", text);
}

void
nxbox_dred_capture(ID3D12Device *dev, HRESULT removed, const char *where, const char *batch_text) {
   if (SUCCEEDED(removed))
      return;
   std::lock_guard<std::mutex> lock(nxbox_dred_mutex);
   auto &state = nxbox_dred_states[dev];
   if (state.captured)
      return;
   state.captured = true;
   NxboxDredText crumbs, page, contexts, batch;
   if (batch_text)
      batch.add("%s", batch_text);
   else
      batch.add("removed=0x%08lx noticed=%s", (unsigned long)removed, where);
   batch.add(" recent(newest-first)=");
   unsigned count = state.next < 4 ? (unsigned)state.next : 4;
   for (unsigned i = 0; i < count; ++i)
      batch.add("[%s]", state.submissions[(state.next - 1 - i) % 4]);
   SetEnvironmentVariableA("NXBOX_D3D12_BATCH", batch.text);
   crumbs.add("removed=0x%08lx at=%s", (unsigned long)removed, where);
   HRESULT hr1 = E_NOINTERFACE, hr = E_NOINTERFACE;
#if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
   ID3D12DeviceRemovedExtendedData1 *dred1 = nullptr;
   hr1 = dev->QueryInterface(IID_PPV_ARGS(&dred1));
   if (SUCCEEDED(hr1)) {
      D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output = {};
      D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
      hr = dred1->GetAutoBreadcrumbsOutput1(&output);
      crumbs.add(" api=1 crumbs_hr=0x%08lx", (unsigned long)hr);
      if (SUCCEEDED(hr))
         nxbox_dred_breadcrumbs(crumbs, contexts, output.pHeadAutoBreadcrumbNode);
      HRESULT page_hr = dred1->GetPageFaultAllocationOutput1(&fault);
      nxbox_dred_pagefault(page, page_hr, fault);
      dred1->Release();
   } else
#endif
   {
#if defined(__ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__)
      ID3D12DeviceRemovedExtendedData *dred = nullptr;
      hr = dev->QueryInterface(IID_PPV_ARGS(&dred));
      if (SUCCEEDED(hr)) {
         D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT output = {};
         D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
         hr = dred->GetAutoBreadcrumbsOutput(&output);
         crumbs.add(" api=0 qi1=0x%08lx crumbs_hr=0x%08lx", (unsigned long)hr1, (unsigned long)hr);
         if (SUCCEEDED(hr))
            nxbox_dred_breadcrumbs(crumbs, contexts, output.pHeadAutoBreadcrumbNode);
         HRESULT page_hr = dred->GetPageFaultAllocationOutput(&fault);
         nxbox_dred_pagefault(page, page_hr, fault);
         page.add(" contexts=unavailable");
         dred->Release();
      } else
#endif
      {
         crumbs.add(" unavailable stage=QueryInterface hr=0x%08lx hr1=0x%08lx", (unsigned long)hr,
                    (unsigned long)hr1);
         char setup[501] = {};
         GetEnvironmentVariableA("NXBOX_D3D12_DRED", setup, sizeof(setup));
         page.add("setup=%s", setup);
      }
   }
   page.add("%s", contexts.text);
   SetEnvironmentVariableA("NXBOX_D3D12_DRED2", page.text);
   SetEnvironmentVariableA("NXBOX_D3D12_DRED", crumbs.text);
}
#endif
