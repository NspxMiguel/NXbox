// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cassert>
#define ASSERT(x) assert(x)
#define ASSERT_MSG(x, ...) assert(x)
#define UNIMPLEMENTED_MSG(...) assert(false)
#define UNREACHABLE_MSG(...) assert(false)
