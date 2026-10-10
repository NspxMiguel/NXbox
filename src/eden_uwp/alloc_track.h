// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

// NXBOX_ALLOC_TRACK=1: the global operator new/delete are replaced by versions that record every
// live allocation of 64 KiB or more with its call site, so the largest consumers of the app's
// memory limit can be named. Disabled (a plain malloc/free) until enabled.
void NxboxAllocTrackEnable();
// "site=<RVA of the caller in the exe> bytes=<live> count=<live>" for the twelve biggest sites.
std::string NxboxAllocTrackReport();
