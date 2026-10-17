// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-layout.h"
typedef struct { uint64_t opened,changed,last; float from,target,current; unsigned selected; int active; } PmDeckMotion;
static inline float pm_deck_ease(uint64_t now,uint64_t start,unsigned duration){
 if(now<start||now-start>=duration)return 1;
 float t=(float)(now-start)/duration,u=1-t;return 1-u*u*u;
}
// Finite motion only: no periodic work after settling, no managed allocation.
static inline int pm_deck_motion_classic(PmDeckMotion* m,PmDeckLayout* l,unsigned selected,uint64_t now){
 float target=l->panels[5].y;
 if(!m->active){*m=(PmDeckMotion){now,now,now,target,target,target,selected,1};}
 if(m->selected!=selected){m->from=m->current;m->target=target;m->changed=now;m->selected=selected;}
 float slide=64*(1-pm_deck_ease(now,m->opened,220));
 m->current=m->from+(m->target-m->from)*pm_deck_ease(now,m->changed,120);
 float dy=m->current-target;
 for(unsigned i=0;i<PM_DECK_LABELS;i++)l->labels[i].x+=slide;
 for(unsigned i=0;i<PM_DECK_PANELS;i++)l->panels[i].x+=slide;
 // Snap the inverse card and its dark text together. Only the independent
 // edge marker travels vertically, so text never crosses a mismatched fill.
 l->panels[8].y+=dy;m->last=now;
 return slide!=0||dy!=0;
}
