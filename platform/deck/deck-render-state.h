// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include "deck-core.h"
// Snapshot copied under the deck mutex, then consumed by the UI callback.
static inline void pm_deck_render_frame(const PmDeck* deck,PmDeckFrame* frame){
 *frame=deck->frame;
 // The local controller owns pane focus between coordinator replies. Do not
 // mutate the received frame or derive pane ownership from toolbar position.
 if(frame->flags&256)frame->browser.active=deck->browserPane;
}
typedef struct { int ready,visible,close;uint64_t heartbeat; } PmDeckRenderState;
static inline PmDeckRenderState pm_deck_render_state(int result,int open,uint32_t sequence,uint64_t now){
 // A valid closed/detached frame proves the UI callback is alive. Waiting for
 // the menu to be visible would deadlock readiness with its opening command.
 return (PmDeckRenderState){result==1,result==1&&open,result==0,result==1&&sequence?now:0};
}
