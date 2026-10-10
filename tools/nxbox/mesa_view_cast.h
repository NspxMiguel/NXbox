/* SPDX-License-Identifier: MIT */
#pragma once

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
