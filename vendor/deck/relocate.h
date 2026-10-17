// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
struct RelocatedCode { unsigned char bytes[256]; size_t size,stolen; };
// Relocate complete x86-64 instructions covering a 14-byte absolute jump.
// Reject unsupported forms and displacements instead of emitting wrong code.
bool BuildTrampoline(const unsigned char* input,size_t available,
                     uint64_t source,uint64_t destination,RelocatedCode* out);
