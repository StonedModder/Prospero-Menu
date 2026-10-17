// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
// Structural container validation only. Imports and module initialization are
// checked by the console loader in the target game. platform: 0=unspecified
// (raw ELF or shared development FSELF), 5=PS5-specific SELF wrapper.
int pm_prx_inspect(const void *data,size_t size,int *platform,char *reason,size_t capacity);
// Adapt only the PS5 moduleparam SDK declaration in a plaintext development
// SELF copy. Original uploads, instructions, imports, PS4 metadata and signed
// containers stay untouched. 1=adapted, 0=not applicable, -1=invalid metadata.
int pm_prx_match_host_sdk(void *data,size_t size,uint32_t hostSdk,uint32_t firmware,uint32_t *original);
uint32_t pm_ps5_process_sdk(const void *data,size_t size);
