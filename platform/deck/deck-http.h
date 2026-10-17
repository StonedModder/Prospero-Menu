// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-core.h"
// Bounded loopback-only transport, called exclusively by the background worker.
// Returns HTTP status, or -1 for an unconfirmed request. Never auto-generates a
// new action identity after an interrupted POST.
static inline int pm_deck_http_has_frame(int status){return status==200||status==202;}
static inline int pm_deck_http_acknowledges(int status){return status==200||(status>=400&&status<600);}
int pm_deck_http(uint16_t port,const char credential[33],const PmDeckEvent* event,const PmDeckFeedback*,PmDeckFrame* frame);

int pm_cheats_http(uint16_t,const char[33],const PmDeckEvent*,const PmDeckFeedback*,PmDeckFrame*);
