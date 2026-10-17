// SPDX-License-Identifier: GPL-3.0-or-later
#include "save-recovery.h"
int pm_save_recovery_flow(const PmSaveRecovery* f){
 PmSaveReceipt receipt;int result=-1;void* c=f->context;
 if(f->acquire(c,&receipt))goto done;
 if((receipt.stage==PM_SAVE_COMPLETE||receipt.stage==PM_SAVE_REJECTED)&&receipt.clean&&!receipt.mounted){result=0;goto done;}
 // Neither an old timestamp nor an error returned by mount proves quiescence.
 if(f->quiescent(c,&receipt)||f->unmounted(c,&receipt))goto done;
 int published=f->published(c,&receipt);if(published<0)goto done;
 result=f->finish(c,&receipt,published==1);
done:f->release(c);return result;
}
