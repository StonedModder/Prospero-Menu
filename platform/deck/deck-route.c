// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck-route.h"
#include <string.h>
static void reset(PmDeck *d){
 memset(d->previous,0,sizeof d->previous);memset(d->chordAt,0,sizeof d->chordAt);
 memset(d->chordActive,0,sizeof d->chordActive);memset(d->chordLatched,0,sizeof d->chordLatched);memset(d->drain,0,sizeof d->drain);
 d->open=d->pending=d->owner=d->presentedOpen=0;d->repeatAt=d->repeatDirection=0;
}
static uint32_t buttons(uint32_t raw){
 uint32_t out=0;const uint32_t from[]={0x80,0x10,0x20,0x40,0x2000,0x4000,0x400,0x800,2,4};
 const uint32_t to[]={PM_LEFT,PM_UP,PM_RIGHT,PM_DOWN,PM_CIRCLE,PM_CROSS,PM_L1,PM_R1,PM_L3,PM_R3};
 for(unsigned i=0;i<10;i++)if(raw&from[i])out|=to[i];return out;
}
void pm_deck_route_focus(PmDeckInput* input,int gameFocused){
 // A failed game bridge is not evidence that ShellUI lost controller focus.
 // Only a verified foreground observation may override that bridge's route.
 if(gameFocused==-2)return;
 if(gameFocused==1&&input->active==1)return;
 input->active=gameFocused==0?0:2;
 input->captured=0;input->captureAt=0;input->owner=-1;
 input->connection=0;input->count=0;input->serial=0;input->deadline=0;
}
void pm_deck_route_tick(PmDeck *d,PmDeckRoute *r,uint64_t now){
 if(r->active==1&&(!r->deadline||now>=r->deadline)){reset(d);r->active=2;r->captured=0;r->resets[3]++;}
}
int pm_deck_route_visible(const PmDeck *d,const PmDeckRoute *r){
 if(!r->active)return d->open;
 if(r->active!=1||!r->captured||!d->open||d->owner>=8)return 0;
 const PmDeckController *c=&r->controllers[d->owner];return c->used&&c->handle==r->owner&&c->connection==r->connection;
}
void pm_deck_route_receive(PmDeck *d,PmDeckRoute *r,const PmDeckInput *input,uint64_t now){
 if(r->active!=input->active||(input->active&&r->epoch!=input->epoch)){
  uint32_t kept[4];memcpy(kept,r->resets,sizeof kept);kept[0]++;
  reset(d);memset(r,0,sizeof *r);r->active=input->active;memcpy(r->resets,kept,sizeof kept);
 }
 // Game capture epochs do not own a dashboard menu or its held shortcut.
 r->epoch=input->epoch;
 r->deadline=input->deadline;r->captureAt=input->captureAt;r->captured=input->captured;r->owner=input->owner;r->connection=input->connection;
 pm_deck_route_tick(d,r,now);if(r->active!=1)return;
 if(!input->serial||input->serial==r->serial)return;
 if(r->serial&&(int32_t)(input->serial-r->serial)<0){reset(d);r->active=2;r->resets[2]++;return;}
 r->serial=input->serial;
 for(unsigned i=0;i<input->count;i++){
  // A batch reaches ShellUI up to one loopback round trip after the game
  // produced it. Drop an event that is too old (or from a skewed clock) on its
  // own: demoting the route here closed an open menu on every slow reply.
  const PmDeckPad *p=&input->events[i];if(p->at>now||now-p->at>PM_DECK_EVENT_WINDOW_MS){r->resets[1]++;continue;}
  int slot=-1;
  for(unsigned j=0;j<8;j++)if(r->controllers[j].used&&r->controllers[j].handle==p->handle){slot=(int)j;break;}
  if(slot>=0&&r->controllers[slot].connection!=p->connection){
   if(d->open&&d->owner==slot){reset(d);r->captured=0;}
   r->controllers[slot]=(PmDeckController){p->handle,p->connection,1,0};d->previous[slot]=d->chordActive[slot]=d->chordLatched[slot]=d->drain[slot]=0;
  }
  if(slot<0)for(unsigned j=0;j<8;j++)if(!r->controllers[j].used||
    ((!d->open||d->owner!=j)&&now>r->controllers[j].at&&now-r->controllers[j].at>2000)){
    slot=(int)j;r->controllers[j]=(PmDeckController){p->handle,p->connection,1,0};d->previous[j]=d->chordActive[j]=d->chordLatched[j]=d->drain[j]=0;break;
  }
  if(slot<0||p->at<r->controllers[slot].at)continue;r->controllers[slot].at=p->at;
  if(d->open&&d->owner!=slot)continue;
  uint32_t mapped=buttons(p->buttons);
  // Opening requests ownership, but cannot select anything until the matching
  // native capture acknowledgement has returned and a render tick presents it.
  if(d->open&&(!pm_deck_route_visible(d,r)||p->at<r->captureAt)){d->previous[slot]=mapped;d->drain[slot]=1;continue;}
  pm_deck_input_at(d,(unsigned)slot,mapped,p->at,now);
 }
}
PmDeckFeedback pm_deck_route_feedback(const PmDeck *d,const PmDeckRoute *r,int ready,uint64_t renderedAt){
 PmDeckFeedback f={.epoch=r->epoch,.serial=r->serial,.open=r->active==1&&d->open,.owner=-1,.ready=!!ready,.renderedAt=renderedAt,.menuOpen=pm_deck_route_visible(d,r)};
 if(f.open&&d->owner<8&&r->controllers[d->owner].used){f.owner=r->controllers[d->owner].handle;f.connection=r->controllers[d->owner].connection;}
 memcpy(f.routeResets,r->resets,sizeof f.routeResets);
 return f;
}
