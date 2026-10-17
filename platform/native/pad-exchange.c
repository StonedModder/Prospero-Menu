// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad-exchange.h"
#include <string.h>
#define AT(field) offsetof(PmPadBridge,field)
static int header(const PmPadBridge *b,uint64_t arena,uint64_t cookie){
 return b->magic==PM_PAD_MAGIC&&b->cookie==cookie&&b->version==1&&b->bytes==sizeof *b&&
  b->moduleEntry==arena&&b->moduleControl==arena+0x4000&&
  b->clock>=0x10000&&b->clock<UINT64_C(0x800000000000)&&b->enabled<=1;
}
int pm_pad_exchange(const PmPadExchangeOps *ops,uint64_t arena,uint64_t cookie,
 const PmPadRequest *request,uint64_t now,PmPadBatch *out){
 if(!out)return 1;memset(out,0,sizeof *out);
 if(!ops||!ops->read||!ops->write||!ops->now||!request||!cookie||!now||now>UINT64_MAX-PM_PAD_LEASE_NS||
  arena<0x10000||arena>UINT64_C(0x800000000000)-0x8000||(arena&0x3fff)||
  !request->epoch||request->mode>PM_PAD_CAPTURE||request->connection>255||
  (request->mode==PM_PAD_CAPTURE&&request->owner<0))return 1;
 PmPadBridge before={0},after={0};
 // Read only the header/cursors, then already-published events. The producer
 // cannot overwrite these slots before we advance consumed.
 if(ops->read(ops->context,0,&before,AT(controllers))||!header(&before,arena,cookie)||
  before.written-before.consumed>PM_PAD_EVENTS||
  (request->mode!=PM_PAD_OFF&&before.faultEpoch==request->epoch))return 1;
 uint32_t reset=before.lease.epoch!=request->epoch||(before.lease.revision&1)||!before.enabled;
 uint32_t count=reset?0:before.written-before.consumed;
 for(uint32_t i=0;i<count;i++){
  uint32_t sequence=before.consumed+i+1;
  PmPadEvent *event=&out->events[i];
  if(ops->read(ops->context,AT(events)+((sequence-1)%PM_PAD_EVENTS)*sizeof *event,event,sizeof *event)||
   event->sequence!=sequence||event->connection>255||event->handle<0||!event->time)goto rejected;
 }
 if(ops->read(ops->context,0,&after,AT(controllers))||!header(&after,arena,cookie)||
  memcmp(&before.lease,&after.lease,sizeof before.lease)||before.consumed!=after.consumed||
  after.written-before.consumed>PM_PAD_EVENTS||before.faultEpoch!=after.faultEpoch||
  before.faults!=after.faults||before.enabled!=after.enabled)goto rejected;
 uint64_t observed=ops->now(ops->context);
 if(observed<now||observed-now>UINT64_C(100000000))goto rejected;
 for(uint32_t i=0;i<count;i++)if(out->events[i].time>observed)goto rejected;
 // Publication has a single coordinator writer (native operation guard).
 // Start with an odd revision even when recovering an interrupted writer.
 PmPadLease lease={0};lease.revision=(before.lease.revision+1)|1;
 lease.epoch=request->epoch;lease.mode=request->mode;lease.owner=request->owner;lease.connection=request->connection;
 lease.expires=request->mode==PM_PAD_OFF?0:now+UINT64_C(400000000);
 if(ops->write(ops->context,AT(lease),&lease.revision,4)||
  ops->write(ops->context,AT(lease)+4,(unsigned char*)&lease+4,sizeof lease-4))goto uncertain;
 lease.revision++;
 if(ops->write(ops->context,AT(lease),&lease.revision,4))goto uncertain;
 PmPadLease verified;
 if(ops->read(ops->context,AT(lease),&verified,sizeof verified)||memcmp(&lease,&verified,sizeof lease))goto uncertain;
 if(ops->write(ops->context,AT(consumed),&before.written,4))goto uncertain;
 // OFF releases the lease but keeps an already-enabled filter alive long
 // enough to drain a held menu press. Bypassing the filter here would leak the
 // closing Circle/X press straight into the game. Idle filtering is bounded.
 uint32_t enabled=request->mode!=PM_PAD_OFF||before.enabled;
 if(ops->write(ops->context,AT(enabled),&enabled,4))goto uncertain;
 // Verify only coordinator-owned fields; the producer remains free to append
 // events and to fault this epoch. Such a fault requires a new epoch upstream.
 if(ops->read(ops->context,0,&after,AT(controllers))||!header(&after,arena,cookie)||
  memcmp(&after.lease,&lease,sizeof lease)||after.consumed!=before.written||after.enabled!=enabled||
  (request->mode!=PM_PAD_OFF&&after.faultEpoch==request->epoch))goto uncertain;
 observed=ops->now(ops->context);if(observed<now||observed-now>UINT64_C(100000000))goto uncertain;
 out->count=count;out->faults=after.faults;out->epoch=request->epoch;out->reset=reset;out->deadline=lease.expires;return 0;
rejected:memset(out,0,sizeof *out);return 1;
uncertain:memset(out,0,sizeof *out);return -1;
}
