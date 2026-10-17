// SPDX-License-Identifier: GPL-3.0-or-later
// Game-thread path: no allocation, I/O, waits or spinlocks. The external
// coordinator grants short leases only after renderer/input readiness checks.
#include "pad-bridge.h"
_Static_assert(sizeof(PmPadSample)==120,"Normal pad sample ABI");
_Static_assert(offsetof(PmPadSample,connected)==76,"Pad connected ABI");
_Static_assert(offsetof(PmPadSample,timestamp)==80,"Pad timestamp ABI");
_Static_assert(offsetof(PmPadSample,connection)==104,"Pad connection ABI");
_Static_assert(sizeof(PmPadEvent)==32,"Pad event wire ABI");
_Static_assert(sizeof(PmPadBridge)<=0x3000,"Pad control reservation");
static int lease(PmPadBridge *b,PmPadLease *l,uint64_t now){
 uint32_t first=__atomic_load_n(&b->lease.revision,__ATOMIC_ACQUIRE);
 if(first&1)return 0;
 // Atomic scalar reads avoid a compiler-generated unbounded/misaligned copy
 // while a coordinator is publishing the next lease revision.
 l->epoch=__atomic_load_n(&b->lease.epoch,__ATOMIC_RELAXED);
 l->mode=__atomic_load_n(&b->lease.mode,__ATOMIC_RELAXED);
 l->owner=__atomic_load_n(&b->lease.owner,__ATOMIC_RELAXED);
 l->connection=__atomic_load_n(&b->lease.connection,__ATOMIC_RELAXED);
 l->expires=__atomic_load_n(&b->lease.expires,__ATOMIC_RELAXED);
 if(first!=__atomic_load_n(&b->lease.revision,__ATOMIC_ACQUIRE)||!l->epoch||l->mode>PM_PAD_CAPTURE||
    l->connection>255||l->expires<=now||l->expires-now>PM_PAD_LEASE_NS)return 0;
 return l->mode!=PM_PAD_OFF&&__atomic_load_n(&b->faultEpoch,__ATOMIC_ACQUIRE)!=l->epoch;
}
static void neutral(PmPadSample *s){
 s->buttons=0;for(unsigned i=0;i<4;i++)s->sticks[i]=128;
 s->triggers[0]=s->triggers[1]=0;
 for(unsigned i=0;i<4;i++)s->orientation[i]=i==3?1.0f:0.0f;
 for(unsigned i=0;i<3;i++)s->acceleration[i]=s->angular[i]=0;
 s->touches[0]=0;for(unsigned i=8;i<24;i++)s->touches[i]=0;
}
static int released(const PmPadSample *s){
 if(s->buttons||s->triggers[0]||s->triggers[1]||s->touches[0])return 0;
 for(unsigned i=0;i<4;i++)if(s->sticks[i]<112||s->sticks[i]>144)return 0;
 return 1;
}
static int usable(const PmPadSample *s){
 return s->connected==1&&!(s->buttons&UINT32_C(0x80000000))&&s->touches[0]<=2&&s->deviceLength<=12&&s->extension[5]<=10;
}
static void fault(PmPadBridge *b,uint32_t epoch){
 __atomic_fetch_add(&b->faults,1,__ATOMIC_RELAXED);
 __atomic_store_n(&b->faultEpoch,epoch,__ATOMIC_RELEASE);
}
static void filter(PmPadBridge *b,int handle,PmPadSample *samples,unsigned count,uint64_t now){
 PmPadLease l={0};int live=lease(b,&l,now);unsigned free=0;
 if(!__atomic_compare_exchange_n(&b->lock,&free,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){
  // Do not let a racing reader leak a menu-owned press into gameplay.
  if(live&&l.mode==PM_PAD_CAPTURE&&handle==l.owner){
   for(unsigned i=0;i<count;i++)if(usable(&samples[i])&&samples[i].connection==l.connection)neutral(&samples[i]);
   fault(b,l.epoch);
  }return;
 }
 PmPadController *c=0;
 for(unsigned i=0;i<PM_PAD_CONTROLLERS;i++)if(b->controllers[i].used&&b->controllers[i].handle==handle){c=&b->controllers[i];break;}
 if(!c)for(unsigned i=0;i<PM_PAD_CONTROLLERS;i++)if(!b->controllers[i].used){c=&b->controllers[i];c->used=1;c->handle=handle;break;}
 if(!c)for(unsigned i=0;i<PM_PAD_CONTROLLERS;i++){
  PmPadController *old=&b->controllers[i];
  if(now>=old->lastSeen&&now-old->lastSeen>UINT64_C(2000000000)&&
     (!old->captured||now>old->captureUntil+PM_PAD_DRAIN_NS)&&now>=old->drainUntil){
   *old=(PmPadController){.handle=handle,.used=1};c=old;break;
  }
 }
 if(!c){if(live)fault(b,l.epoch);goto done;}
 c->lastSeen=now;
 for(unsigned i=0;i<count;i++){
  PmPadSample *s=&samples[i];
  if(!usable(s)){c->captured=0;c->drainUntil=0;c->timestamp=0;continue;}
  if(c->connection!=s->connection){c->connection=s->connection;c->captured=0;c->drainUntil=0;c->timestamp=0;c->lastEvent=0;}
  // ReadState can delegate to another hooked read. Do not publish the neutral
  // buffer returned by the inner hook as a physical release in the outer one.
  if(s->timestamp&&c->timestamp==s->timestamp&&(c->captured||c->drainUntil)&&released(s))continue;
  int capture=live&&l.mode==PM_PAD_CAPTURE&&handle==l.owner&&s->connection==l.connection;
  int changed=!c->lastEvent||c->buttons!=s->buttons;
  for(unsigned j=0;j<4;j++)if(c->sticks[j]!=s->sticks[j])changed=1;
  for(unsigned j=0;j<2;j++)if(c->triggers[j]!=s->triggers[j])changed=1;
  // A cached ReadState sample may keep the same input timestamp. Emit an
  // occasional keepalive using the monotonic clock for chord/repeat timing.
  if(live&&(changed||now-c->lastEvent>=UINT64_C(100000000))){
   uint32_t written=__atomic_load_n(&b->written,__ATOMIC_RELAXED),consumed=__atomic_load_n(&b->consumed,__ATOMIC_ACQUIRE);
   if(written-consumed>=PM_PAD_EVENTS){fault(b,l.epoch);live=0;}
   else {
    uint32_t seq=written+1;PmPadEvent *e=&b->events[written%PM_PAD_EVENTS];
    e->sequence=seq;e->buttons=s->buttons;e->handle=handle;e->connection=s->connection;e->time=now;
    for(unsigned j=0;j<4;j++)e->sticks[j]=s->sticks[j];for(unsigned j=0;j<2;j++)e->triggers[j]=s->triggers[j];
    __atomic_store_n(&b->written,seq,__ATOMIC_RELEASE);c->lastEvent=now;
   }
  }
  c->buttons=s->buttons;c->timestamp=s->timestamp;
  for(unsigned j=0;j<4;j++)c->sticks[j]=s->sticks[j];for(unsigned j=0;j<2;j++)c->triggers[j]=s->triggers[j];
  if(capture){c->captured=1;c->captureUntil=l.expires;c->drainUntil=0;neutral(s);}
  else {
   if(c->captured){c->captured=0;c->drainUntil=(now<c->captureUntil?now:c->captureUntil)+PM_PAD_DRAIN_NS;}
   if(c->drainUntil){if(now>=c->drainUntil||released(s))c->drainUntil=0;else neutral(s);}
  }
 }
done:__atomic_store_n(&b->lock,0,__ATOMIC_RELEASE);
}
__attribute__((section(".text.entry"),noinline))
int PM_GAME_SYSV pm_pad_bridge(PmPadBridge *b,uint64_t cookie,unsigned lane,uint64_t original,int handle,PmPadSample *samples,int capacity){
 // The trampoline supplies the immutable original entry, so even a corrupt
 // bridge header does not strand input or redirect a call through its data.
 if(!original)return -1;
 int valid=b&&b->magic==PM_PAD_MAGIC&&b->version==1&&b->bytes==sizeof *b&&cookie&&cookie==b->cookie&&lane<2;
 if(valid&&b->moduleEntry&&b->moduleControl)
  ((void(PM_GAME_SYSV *)(PmGameGate*,uint64_t,unsigned))(uintptr_t)b->moduleEntry)((PmGameGate*)(uintptr_t)b->moduleControl,cookie,lane);
 int result=lane==0?((PmPadReadState)(uintptr_t)original)(handle,samples):((PmPadRead)(uintptr_t)original)(handle,samples,capacity);
 if(!valid||!__atomic_load_n(&b->enabled,__ATOMIC_ACQUIRE)||!b->clock||!samples||handle<0)return result;
 unsigned count=lane==0?(result==0?1:0):(capacity>=1&&capacity<=64&&result>0&&result<=capacity?(unsigned)result:0);
 if(!count)return result;
 PmPadTime time={0};if(((PmPadClock)(uintptr_t)b->clock)(4,&time)||time.sec<0||time.sec>INT64_C(18000000000)||time.nsec<0||time.nsec>=1000000000)return result;
 uint64_t now=(uint64_t)time.sec*UINT64_C(1000000000)+(uint64_t)time.nsec;
 filter(b,handle,samples,count,now);return result;
}
