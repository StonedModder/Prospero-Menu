// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-core.h"
// Oldest controller event the resident still applies, in console milliseconds.
#define PM_DECK_EVENT_WINDOW_MS 400u
typedef struct {int32_t handle;uint32_t connection,used;uint64_t at;} PmDeckController;
typedef struct {
 uint32_t epoch,serial,active,captured;uint64_t deadline,captureAt;
 int32_t owner;uint32_t connection;PmDeckController controllers[8];
 uint32_t resets[4]; // ProsperoMenu diagnostics: route change, stale event, serial regression, deadline
} PmDeckRoute;
void pm_deck_route_receive(PmDeck*,PmDeckRoute*,const PmDeckInput*,uint64_t now);
void pm_deck_route_tick(PmDeck*,PmDeckRoute*,uint64_t now);
void pm_deck_route_focus(PmDeckInput*,int gameFocused);
int pm_deck_route_visible(const PmDeck*,const PmDeckRoute*);
PmDeckFeedback pm_deck_route_feedback(const PmDeck*,const PmDeckRoute*,int ready,uint64_t renderedAt);
