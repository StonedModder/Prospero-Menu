// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "save-core.h"
typedef struct {
 void* context;
 int (*record)(void*,unsigned stage,int clean,int mounted,const char* message);
 int (*prepare)(void*);int (*copy)(void*);int (*mount)(void*);
 int (*extract)(void*);int (*unmount)(void*);int (*publish)(void*);
 void (*discard)(void*);
} PmSaveFlow;
// Once a mount call could have happened, extraction/publishing may proceed only
// through a confirmed unmount. Unknown operations are never replayed here.
int pm_save_export_flow(const PmSaveFlow* flow);
