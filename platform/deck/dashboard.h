// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-core.h"
#include "mono-api.h"
#include "deck-bootstrap.h"
// Optional dashboard hooks; a metadata mismatch never rejects the main menu.
int pm_dashboard_prepare(PmMono*,void*,void* targets[3],void* replacements[3]);
unsigned pm_dashboard_prepare_error(void);
void pm_dashboard_originals(void* trampolines[3]);
void pm_dashboard_receive(const PmDashboardFrame*);
void pm_dashboard_feedback(PmDeckFeedback*);
int pm_dashboard_ack(uint32_t);

void pm_dashboard_bind(volatile PmDeckBoot*);

void pm_dashboard_tick(int idle);

int pm_dashboard_version_prepare(void**,void**);
void pm_dashboard_version_original(void*);
