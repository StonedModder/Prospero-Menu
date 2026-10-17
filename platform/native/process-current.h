// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Caller owns the native operation guard. Require agreement between an explicit
// live-list walk and the SDK entry point (wrapped by process-fresh.c in all
// console artifacts). This is not a substitute for a stopped generation lease.
int pm_process_current(int pid);
