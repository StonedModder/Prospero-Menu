// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck-core.h"
void pm_deck_metrics_focus(PmDeckFrame* frame,int gameFocused){
 // Focus changes invalidate the HUD even when the sampled numbers did not.
 frame->metricsSequence=(frame->metricsSequence<<1)|(gameFocused==1);
 if(gameFocused!=1)frame->metrics[0]=0;
}
#include <string.h>
static int text_valid(const char* text,size_t n){
 const unsigned char* p=(const unsigned char*)text;size_t i=0;
 while(i<n&&p[i]){
  unsigned c=p[i++],need=0,cp;
  if(c<32||c==127)return 0;
  if(c<128)continue;
  if(c>=0xc2&&c<=0xdf){need=1;cp=c&31;}
  else if(c>=0xe0&&c<=0xef){need=2;cp=c&15;}
  else if(c>=0xf0&&c<=0xf4){need=3;cp=c&7;}else return 0;
  if(i+need>=n)return 0;
  for(unsigned j=0;j<need;j++){unsigned b=p[i++];if((b&0xc0)!=0x80)return 0;cp=(cp<<6)|(b&63);}
  if((need==1&&cp<0x80)||(need==2&&cp<0x800)||(need==3&&cp<0x10000)||
     (cp>=0xd800&&cp<=0xdfff)||cp>0x10ffff)return 0;
 }
 return i<n;
}
int pm_deck_decode(const void* bytes,size_t length,PmDeckFrame* out){
  if(!bytes||!out||length!=sizeof(*out))return 0;
  PmDeckFrame f;memcpy(&f,bytes,sizeof f);
  if(f.magic!=PM_DECK_MAGIC||f.version!=PM_DECK_VERSION||(f.flags&~511u)||(((f.flags>>3)&3)==3)||!f.sequence||f.page>=6||f.count>PM_DECK_ROWS||!f.metricsSequence||!text_valid(f.metrics,sizeof f.metrics)||
    !text_valid(f.title,sizeof f.title)||!text_valid(f.subtitle,sizeof f.subtitle)||
    !text_valid(f.footer,sizeof f.footer)||!text_valid(f.ip,sizeof f.ip)||
    !text_valid(f.logoPath,sizeof f.logoPath))return 0;
  // A window must sit inside the list, and a moved window always ships full.
  if(f.reserved0||f.count>f.total||(f.windowStart&&(f.count!=PM_DECK_ROWS||f.windowStart+f.count>f.total))||
     (f.focus!=PM_DECK_FOCUS_NONE&&f.focus>=f.total))return 0;
 if(!f.notice.sequence||f.notice.kind>2||f.notice.remainingMs>9000||f.notice.count>999||
    !text_valid(f.notice.title,sizeof f.notice.title)||!text_valid(f.notice.detail,sizeof f.notice.detail)||
    (!f.notice.kind&&(f.notice.remainingMs||f.notice.count||f.notice.title[0]||f.notice.detail[0]))||
    (f.notice.kind&&(!f.notice.remainingMs||!f.notice.count||!f.notice.title[0])))return 0;
 if((f.flags&128)&&((f.flags&32)||f.count!=1||strlen(f.rows[0].label)!=36))return 0;
 if(f.flags&256){
  if((f.flags&(32|128))||f.count!=8||f.browser.active>1||f.browser.reserved||!text_valid(f.browser.status,sizeof f.browser.status))return 0;
  for(unsigned i=0;i<2;i++)if(!text_valid(f.browser.paths[i],160))return 0;
  for(unsigned i=0;i<24;i++)if((f.browser.entries[i].flags&~7u)||!text_valid(f.browser.entries[i].label,96)||!text_valid(f.browser.entries[i].detail,64))return 0;
 }
 for(unsigned i=0;i<f.count;i++){
  const PmDeckRow* row=&f.rows[i];
  for(unsigned j=0;j<32;j++)if(!((row->token[j]>='0'&&row->token[j]<='9')||(row->token[j]>='a'&&row->token[j]<='f')))return 0;
  if((row->flags&~(uint32_t)PM_ROW_FLAGS)||!text_valid(row->label,sizeof row->label)||!text_valid(row->detail,sizeof row->detail))return 0;
  // A right-aligned value follows the label's terminator inside the same field.
  if(row->flags&PM_ROW_VALUE){size_t n=strlen(row->label)+1;if(n>=sizeof row->label||!text_valid(row->label+n,sizeof row->label-n))return 0;}
  if((row->flags&PM_ROW_HEADER)&&(row->flags&(PM_ROW_ENABLED|PM_ROW_ADJUST)))return 0;
 }
 if(f.theme.reserved)return 0;
 if(f.reserved||f.input.reserved||f.input.active>2||f.input.captured>1||f.input.count>128||f.input.connection>255||
  (f.input.active==1&&(!f.input.epoch||!f.input.deadline))||
  (f.input.captured&&(f.input.active!=1||f.input.owner<0||!f.input.captureAt||f.input.captureAt>=f.input.deadline))||
  (f.input.count&&(!f.input.serial||f.input.active!=1)))return 0;
 for(unsigned i=0;i<f.input.count;i++)if(f.input.events[i].handle<0||f.input.events[i].connection>255||!f.input.events[i].at)return 0;
 if(f.dashboard.enabled>1||f.dashboard.count>32)return 0;
 for(unsigned i=0;i<f.dashboard.count;i++){const PmDashboardRecord*r=&f.dashboard.records[i];if(r->reserved||!text_valid(r->title,10)||!text_valid(r->version,16))return 0;}
 *out=f;return 1;
}
void pm_deck_init(PmDeck* d){memset(d,0,sizeof(*d));d->focusCommand=PM_DECK_FOCUS_NONE;}
// Category headers are labels, not stops: focus never rests on one while a
// focusable row exists in the shipped window.
static int pm_header(const PmDeck* d,unsigned absolute){
 unsigned i=absolute-d->frame.windowStart;
 return absolute>=d->frame.windowStart&&i<d->frame.count&&(d->frame.rows[i].flags&PM_ROW_HEADER);
}
static void pm_deck_settle(PmDeck* d){
 if(d->frame.flags&(128|256u))return;
 unsigned start=d->frame.windowStart,end=start+d->frame.count,f=d->focus;
 while(f<end&&pm_header(d,f))f++;
 if(f>=end){f=d->focus;while(f>start&&pm_header(d,f))f--;}
 if(f<end&&!pm_header(d,f)){d->focus=f;d->selected=(uint8_t)(f-start);}
}
static void pm_deck_window_refill(PmDeck* d);
int pm_deck_receive(PmDeck* d,const void* bytes,size_t length,uint64_t now){
 PmDeckFrame f;if(!pm_deck_decode(bytes,length,&f))return 0;
 if(d->valid&&(int32_t)(f.sequence-d->frame.sequence)<0)return 0;
 if(d->valid&&f.sequence==d->frame.sequence&&memcmp(&f,&d->frame,offsetof(PmDeckFrame,metricsSequence)))return 0;
 if(d->valid&&f.sequence==d->frame.sequence&&memcmp(&f.browser,&d->frame.browser,sizeof f.browser))return 0;
 if(d->valid&&((int32_t)(f.metricsSequence-d->frame.metricsSequence)<0||
    (f.metricsSequence==d->frame.metricsSequence&&memcmp(f.metrics,d->frame.metrics,sizeof f.metrics))))return 0;
 if(d->frame.notice.sequence){
  if((int32_t)(f.notice.sequence-d->frame.notice.sequence)<0)return 0;
  if(f.notice.sequence==d->frame.notice.sequence&&memcmp(&f.notice.kind,&d->frame.notice.kind,sizeof(f.notice)-offsetof(PmDeckNotice,kind)))return 0;
 }
 uint64_t expires=now+f.notice.remainingMs;if(expires<now)return 0;
 if(f.notice.sequence!=d->frame.notice.sequence)d->noticeDeadline=expires;
 else if(expires<d->noticeDeadline)d->noticeDeadline=expires;
  unsigned count=(f.flags&128)?40:(f.flags&256)?32:f.count;
  unsigned window=f.windowStart;
  int fresh=!d->valid||f.page!=d->frame.page||strcmp(f.title,d->frame.title);
  // An explicit focus is a one-shot command. It always applies on a fresh view,
  // but on later frames of the same view only when the value CHANGED: a
  // coordinator that keeps asserting a stale "return focus" (the root list
  // does) must never undo the user's scrolling.
  int command=f.focus!=PM_DECK_FOCUS_NONE&&(fresh||f.focus!=d->focusCommand);
  if(f.focus!=PM_DECK_FOCUS_NONE)d->focusCommand=f.focus;
  if(command)d->focus=(f.focus>=window&&f.focus<window+count)?f.focus:window;
  // Refills and background rebuilds keep the focused item while it stays in
  // the shipped window; anything else snaps to the window's first row.
  else if(fresh||d->focus<window||d->focus>=window+count)d->focus=window;
  d->selected=count?(uint8_t)(d->focus-window):0;
  if(d->selected>=count)d->selected=count?count-1:0;
  d->frame=f;d->received=now;d->valid=1;
  pm_deck_settle(d);
  if(!(f.flags&(128|256u)))pm_deck_window_refill(d);
  return 1;
}
int pm_deck_stalled(const PmDeck* d,uint64_t now){
 return d->valid&&(now<d->received||now-d->received>PM_DECK_STALE_MS);
}
void pm_deck_tick(PmDeck* d,uint64_t now){
 // A slow coordinator is not a failed renderer. Retain an already open menu
 // briefly, with actions paused and a local exit, while its worker reconnects.
 uint64_t deadline=d->open?PM_DECK_DISCONNECTED_MS:PM_DECK_STALE_MS;
 if(d->valid&&(now<d->received||now-d->received>deadline||!(d->frame.flags&1))){
  if(d->open)d->drain[d->owner]=1;d->open=0;d->valid=0;
 }
}
uint32_t pm_deck_notice_remaining(const PmDeck* d,uint64_t now){
 if(!d->valid||!(d->frame.flags&1)||!d->frame.notice.kind||now<d->received||now-d->received>PM_DECK_STALE_MS||now>=d->noticeDeadline)return 0;
 uint64_t remaining=d->noticeDeadline-now;return (uint32_t)(remaining>9000?9000:remaining);
}
void pm_deck_present(PmDeck* d,uint32_t sequence,int open,unsigned selected){
 if(!d->valid||sequence!=d->frame.sequence)return;
 d->presentedSequence=sequence;d->presentedOpen=!!open;d->presentedSelected=(uint8_t)selected;
}
static void action(PmDeck* d,unsigned kind){
 if(d->pending)return;
 if(++d->serial==0){d->valid=0;d->open=0;return;} // no request ID reuse
 d->event=(PmDeckEvent){kind,d->frame.sequence,d->selected,d->serial};d->pending=1;
}
// Ask the coordinator to recentre the 32-row window on the focused item, like
// libtesla's List keeps its focus a third of the way down. The request rides
// the regular sync poll, one event at a time like every other action.
static void pm_deck_window_refill(PmDeck* d){
 if(d->pending||!(d->frame.flags&1))return;
 unsigned total=d->frame.total;if(total<=PM_DECK_ROWS)return;
 unsigned count=d->frame.count,start=d->frame.windowStart,desired=start;
 if((d->focus<start+4&&start)||(d->focus+4>=start+count&&start+count<total))
  desired=d->focus>PM_DECK_WINDOW_LEAD?d->focus-PM_DECK_WINDOW_LEAD:0;
 else return;
 if(desired>total-PM_DECK_ROWS)desired=total-PM_DECK_ROWS;
 if(desired==start)return;
 action(d,PM_DECK_WINDOW);d->event.row=desired;
}
// One item at a time, clamped at both ends; the list never wraps.
static int pm_deck_step(PmDeck* d,int down){
 unsigned total=d->frame.total,start=d->frame.windowStart,count=d->frame.count,f=d->focus;
 // Skip category headers; stay put when only headers remain in that direction.
 do{
  if(down){if(f+1u>=total||f+1u>=start+count)return 0;f++;}
  else{if(!f||f<=start)return 0;f--;}
 }while(pm_header(d,f));
 d->focus=f;d->selected=(uint8_t)(f-start);return 1;
}
// L1/R1 jump one viewport. The jump stops at the window edge until the
// requested refill arrives, so focus never lands on an unshipped row.
static void pm_deck_page(PmDeck* d,int down){
 unsigned total=d->frame.total,start=d->frame.windowStart,count=d->frame.count;
 if(!total||!count)return;
 unsigned target=down?(d->focus<total-PM_DECK_VIEWPORT_ROWS?d->focus+PM_DECK_VIEWPORT_ROWS:total-1)
  :(d->focus>PM_DECK_VIEWPORT_ROWS?d->focus-PM_DECK_VIEWPORT_ROWS:0);
 if(target<start)target=start;
 if(target>=start+count)target=start+count-1;
 d->focus=target;d->selected=(uint8_t)(target-start);
 pm_deck_settle(d);
 pm_deck_window_refill(d);
}
int pm_deck_input_at(PmDeck* d,unsigned device,uint32_t buttons,uint64_t now,uint64_t receivedNow){
 if(device>=8||now>receivedNow||receivedNow-now>400)return 0;pm_deck_tick(d,receivedNow);
 uint32_t pressed=buttons&~d->previous[device];d->previous[device]=buttons;
 const uint32_t chord=PM_L3|PM_R3;
 if((buttons&chord)!=chord){d->chordActive[device]=0;d->chordLatched[device]=0;}
 else if(!d->chordLatched[device]&&(!d->open||d->owner==device)){
  if(!d->chordActive[device]){d->chordActive[device]=1;d->chordAt[device]=now;}
  if(now>=d->chordAt[device]&&now-d->chordAt[device]>=350){
   d->chordLatched[device]=1;
   if(d->valid&&(d->open||!pm_deck_stalled(d,receivedNow))){d->open=!d->open;d->owner=device;d->drain[device]=1;d->repeatDirection=0;}
  }
 }
 if(d->drain[device]){if(!buttons)d->drain[device]=0;return 1;}
 if(!d->open)return 0;if(d->owner!=device)return 1;
 if(pm_deck_stalled(d,receivedNow)){
  // Never execute stale rows or strand the user behind a pending HTTP action.
  if(pressed&PM_CIRCLE){d->open=0;d->drain[device]=1;}
  return 1;
 }
 // The worker may receive a replacement frame between two render ticks.
 // Never activate a row whose text/highlight the user has not actually seen.
 if(d->presentedSequence!=d->frame.sequence||!d->presentedOpen||d->presentedSelected!=d->selected){
  // A released D-pad still ends key repeat while the next frame is drawn.
  if(!(buttons&(PM_UP|PM_DOWN|PM_LEFT|PM_RIGHT)))d->repeatDirection=0;
  return 1;
 }
 if(pressed&PM_CIRCLE){
  if(d->pending)return 1;
  if(d->frame.page==0&&!(d->frame.flags&4)&&!d->pending){d->open=0;d->drain[device]=1;}
 else {action(d,PM_DECK_BACK);if((d->frame.flags&256)&&d->selected>=24)d->event.row+=32*d->browserPane;}return 1;
 }
 if(d->pending&&d->event.kind!=PM_DECK_WINDOW)return 1;
 // Focus keeps moving inside the current window while a refill is in flight;
 // it only stops at the window edge until the new window arrives.
 if(d->frame.flags&256){
  unsigned n=d->selected;
  if(pressed&PM_L1)d->selected=n<24?n%12:0;
  else if(pressed&PM_R1)d->selected=12+(n<24?n%12:0);
  else if(pressed&PM_CROSS){if(n<24?(d->frame.browser.entries[n].flags&1):(d->frame.rows[n-24].flags&1)){action(d,PM_DECK_SELECT);if(n>=24)d->event.row+=32*d->browserPane;}}
  else{
   unsigned direction=buttons&(PM_UP|PM_DOWN|PM_LEFT|PM_RIGHT);
   if(direction==PM_UP||direction==PM_DOWN||direction==PM_LEFT||direction==PM_RIGHT){
    if(direction!=d->repeatDirection||now>=d->repeatAt){
     if(n<24){unsigned side=n/12,row=n%12;
      if(direction==PM_LEFT)d->selected=row;
      else if(direction==PM_RIGHT)d->selected=12+row;
      else {int step=direction==PM_UP?-1:1,next=(int)row+step;
       while(next>=0&&next<12&&!(d->frame.browser.entries[side*12+next].flags&1))next+=step;
       d->selected=next>=0&&next<12?side*12+next:24+side*4;
      }
     }else if(direction==PM_LEFT)d->selected=24+(n-24+7)%8;
     else if(direction==PM_RIGHT)d->selected=24+(n-24+1)%8;
     else d->selected=(n>=28?12:0)+(direction==PM_UP?11:0);
     d->repeatAt=now+(direction!=d->repeatDirection?350:120);
    }
   }d->repeatDirection=direction;
  }if(d->selected<24){
   unsigned side=d->selected/12,row=d->selected%12;
   if(!(d->frame.browser.entries[d->selected].flags&1)){
    unsigned found=24;for(unsigned distance=1;distance<12;distance++){
     if(row+distance<12&&(d->frame.browser.entries[side*12+row+distance].flags&1)){found=side*12+row+distance;break;}
     if(row>=distance&&(d->frame.browser.entries[side*12+row-distance].flags&1)){found=side*12+row-distance;break;}
    }if(found<24)d->selected=found;
    }d->browserPane=side;
   }d->focus=d->selected;return 1;
 }
 if(d->frame.flags&128){
  // Text input keeps ProsperoMenu's 40-key grid; L1/R1 cycle the abc/ABC/symbols sets.
  if(pressed&PM_R1)action(d,PM_DECK_NEXT);
  else if(pressed&PM_L1)action(d,PM_DECK_PREVIOUS);
  else if(pressed&PM_CROSS)action(d,PM_DECK_SELECT);
  else{
   unsigned direction=buttons&(PM_UP|PM_DOWN|PM_LEFT|PM_RIGHT);
   if(direction==PM_UP||direction==PM_DOWN||direction==PM_LEFT||direction==PM_RIGHT){
    if(direction!=d->repeatDirection||now>=d->repeatAt){
     unsigned step=direction==PM_UP||direction==PM_DOWN?10:1;
     d->selected=(uint8_t)((d->selected+((direction==PM_DOWN||direction==PM_RIGHT)?step:40-step))%40);
     d->repeatAt=now+(direction!=d->repeatDirection?400:80);
    }
   }d->repeatDirection=direction;
  }
  d->focus=d->selected;return 1;
 }
 #ifdef PM_CLASSIC_DECK
 if(d->frame.flags&32){
  // Classic centred window only: Home stays upstream's 2x4 tile grid.
  unsigned direction=buttons&(PM_UP|PM_DOWN|PM_LEFT|PM_RIGHT);
  if(direction==PM_UP||direction==PM_DOWN||direction==PM_LEFT||direction==PM_RIGHT){
   if(direction!=d->repeatDirection||now>=d->repeatAt){
    unsigned count=d->frame.count;
    if(count){unsigned step=direction==PM_UP||direction==PM_DOWN?4:1;
     d->selected=(uint8_t)((d->selected+((direction==PM_DOWN||direction==PM_RIGHT)?step:count-step%count))%count);}
    d->repeatAt=now+(direction!=d->repeatDirection?350:120);
   }
  }d->repeatDirection=direction;d->focus=d->selected;return 1;
 }
 #endif
 if(pressed&PM_R1)pm_deck_page(d,1);
 else if(pressed&PM_L1)pm_deck_page(d,0);
 else if(pressed&PM_CROSS){if(d->frame.count&&(d->frame.rows[d->selected].flags&1))action(d,PM_DECK_SELECT);}
 else {
  // Tesla list: focus moves one item at a time and the list never wraps; a
  // held D-pad repeats after 400 ms, then every 80 ms.
  // Left/Right adjust a focused slider or picker row (PM_ROW_ADJUST); the
  // coordinator owns the value, so each step is one NEXT/PREVIOUS event.
  unsigned direction=buttons&(PM_UP|PM_DOWN|PM_LEFT|PM_RIGHT);
  int adjust=d->frame.count&&(d->frame.rows[d->selected].flags&(PM_ROW_ADJUST|PM_ROW_ENABLED))==(PM_ROW_ADJUST|PM_ROW_ENABLED);
  if(direction==PM_UP||direction==PM_DOWN){
   if(direction!=d->repeatDirection||now>=d->repeatAt){
    int moved=pm_deck_step(d,direction==PM_DOWN);
    pm_deck_window_refill(d);
    // Held at a window edge: move again as soon as the refill arrives.
    d->repeatAt=moved?now+(direction!=d->repeatDirection?400:80):now;
   }
  }else if((direction==PM_LEFT||direction==PM_RIGHT)&&adjust){
   if((direction!=d->repeatDirection||now>=d->repeatAt)&&!d->pending){
    action(d,direction==PM_RIGHT?PM_DECK_NEXT:PM_DECK_PREVIOUS);
    d->repeatAt=now+(direction!=d->repeatDirection?400:120);
   }
  }
  d->repeatDirection=direction;
 }
 return 1;
}
int pm_deck_input(PmDeck* d,unsigned device,uint32_t buttons,uint64_t now){return pm_deck_input_at(d,device,buttons,now,now);}
int pm_deck_event(const PmDeck* d,PmDeckEvent* event){if(!d->pending)return 0;*event=d->event;return 1;}
void pm_deck_ack(PmDeck* d,uint32_t serial){if(d->pending&&d->event.serial==serial)d->pending=0;}
