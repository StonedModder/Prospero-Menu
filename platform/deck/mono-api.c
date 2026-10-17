// SPDX-License-Identifier: GPL-3.0-or-later
#include "mono-api.h"
#include <ps5/kernel.h>
#include <unistd.h>
#include <string.h>
int pm_mono_load(PmMono* api){
 uint32_t handle=0;memset(api,0,sizeof *api);
 // Read-only lookup; never temporarily replace ShellUI's process credentials.
 if(kernel_dynlib_handle(getpid(),"libmonosgen-2.0.sprx",&handle))return 0;
#define RESOLVE(ret,name,args) api->name=(ret (*) args)kernel_dynlib_dlsym(getpid(),handle,#name);if(!api->name)return 0;
 PM_MONO_API(RESOLVE)
#undef RESOLVE
 // Resolve optional dashboard exports from the same actual Mono module.
 // Missing extras must never prevent the menu renderer from loading.
#define RESOLVE_OPTIONAL(ret,name,args) api->name=(ret (*) args)kernel_dynlib_dlsym(getpid(),handle,#name);
 PM_MONO_OPTIONAL_API(RESOLVE_OPTIONAL)
 PM_MONO_PROBE_API(RESOLVE_OPTIONAL)
#undef RESOLVE_OPTIONAL
 return 1;
}
