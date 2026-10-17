// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROSPEROMENU_PROCESS_INFO_H
#define PROSPEROMENU_PROCESS_INFO_H
#include <sys/types.h>
#include <sys/user.h>
int pm_process_info(pid_t pid,struct kinfo_proc* out);
#endif
