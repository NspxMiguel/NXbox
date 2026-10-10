/* SPDX-License-Identifier: MIT */
#pragma once

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

inline bool
nxbox_srv_swizzle_diagnostic_enabled()
{
   static const bool enabled = []()
   {
      const char *value = getenv("NXBOX_SRV_SWIZZLE");
      return value && !strcmp(value, "1");
   }();
   return enabled;
}

inline void
nxbox_record_swizzle_slots(unsigned start_slot)
{
   if (!nxbox_srv_swizzle_diagnostic_enabled())
      return;
   static std::mutex mutex;
   std::lock_guard<std::mutex> lock(mutex);
   static unsigned binds = 0, partial = 0;
   ++binds;
   partial += start_slot != 0;
   /* Publish the first bind and partial bind, then at powers of two. */
   if ((binds & (binds - 1)) && !(start_slot && partial == 1))
      return;
   char text[96];
   snprintf(text, sizeof(text), "binds=%u partial=%u last_start=%u", binds, partial, start_slot);
   SetEnvironmentVariableA("NXBOX_D3D12_SWIZZLE_SLOTS", text);
}

/* Opt-in, bounded descriptor evidence. Paths: native=0, shadow=1, fallback=2.
 * Mapping is the actual D3D12 SRV mapping, including constants and format composition.
 * A descriptor record proves the requested mapping, not that the GPU honored it. */
inline void
nxbox_record_srv_swizzle(unsigned view, unsigned resource, unsigned dxgi, unsigned mapping,
                         unsigned path)
{
   if (!nxbox_srv_swizzle_diagnostic_enabled())
      return;
   static std::mutex mutex;
   std::lock_guard<std::mutex> lock(mutex);
   static unsigned records[16][5] = {};
   static unsigned count = 0;
   const unsigned record[5] = {view, resource, dxgi, mapping, path};
   for (unsigned i = 0; i < count; ++i)
      if (!memcmp(records[i], record, sizeof(record)))
         return;
   if (count == 16)
      return;
   memcpy(records[count++], record, sizeof(record));
   char text[1536] = {};
   size_t used = 0;
   for (unsigned i = 0; i < count; ++i)
      used += snprintf(text + used, sizeof(text) - used,
                       "%sview=%u resource=%u dxgi=%u map=0x%x path=%u", i ? "; " : "",
                       records[i][0], records[i][1], records[i][2], records[i][3], records[i][4]);
   SetEnvironmentVariableA("NXBOX_D3D12_SRV_SWIZZLE", text);
}

/* External inline linkage shares these diagnostics across driver translation units. */
inline void
nxbox_count_view_cast(const char *kind, unsigned view_format, unsigned resource_format)
{
   static std::mutex mutex;
   std::lock_guard<std::mutex> lock(mutex);
   static unsigned srv = 0, srv_copy = 0, rtv = 0, dsv = 0, uav = 0;
   static unsigned pairs[16][2] = {};
   static unsigned num_pairs = 0;
   if (!strcmp(kind, "srv_copy"))
      ++srv_copy;
   else if (!strcmp(kind, "srv"))
      ++srv;
   else if (!strcmp(kind, "rtv"))
      ++rtv;
   else if (!strcmp(kind, "uav"))
      ++uav;
   else
      ++dsv;

   unsigned i;
   for (i = 0; i < num_pairs; ++i)
      if (pairs[i][0] == view_format && pairs[i][1] == resource_format)
         break;
   if (i == num_pairs && num_pairs < 16) {
      pairs[num_pairs][0] = view_format;
      pairs[num_pairs++][1] = resource_format;
   }
   char text[512];
   snprintf(text, sizeof(text), "srv=%u srv_copy=%u rtv=%u dsv=%u uav=%u last=%s:%u->%u", srv,
            srv_copy, rtv, dsv, uav, kind, view_format, resource_format);
   SetEnvironmentVariableA("NXBOX_D3D12_VIEW_CAST", text);
   size_t used = 0;
   text[0] = '\0';
   for (i = 0; i < num_pairs; ++i)
      used += snprintf(text + used, sizeof(text) - used, "%s%u->%u", i ? " " : "", pairs[i][0],
                       pairs[i][1]);
   SetEnvironmentVariableA("NXBOX_D3D12_VIEW_CAST_PAIRS", text);
}

inline bool
nxbox_view_cast_copy_enabled()
{
   static const bool enabled = []() {
      const char *value = getenv("NXBOX_VIEW_CAST_COPY");
      return !value || strcmp(value, "0") != 0;
   }();
   return enabled;
}
