// SPDX-License-Identifier: GPL-3.0-or-later
#include "save-flow.h"
int pm_save_export_flow(const PmSaveFlow* f){
 void* c=f->context;const char* failure="Save export preflight failed";
 if(f->record(c,PM_SAVE_PREPARING,1,0,"Checking the save, closed game and destination"))return -1;
 if(f->prepare(c))goto rejected;
 if(f->record(c,PM_SAVE_COPYING,1,0,"Copying and verifying the encrypted save"))goto rejected;
 failure="Encrypted copy could not be verified";if(f->copy(c))goto rejected;
 if(f->record(c,PM_SAVE_MOUNTING,0,1,"Mounting the verified disposable save copy"))return -1;
 failure="Save mount did not complete";int failed=f->mount(c);
 if(!failed){
  failed=f->record(c,PM_SAVE_EXTRACTING,0,1,"Extracting and verifying save files");
  failure="Save extraction did not complete";if(!failed)failed=f->extract(c);
 }
 // Also reconcile a mount call that returned an error. The platform adapter
 // must verify the mount is absent; a failed API call alone proves nothing.
 if(f->record(c,PM_SAVE_UNMOUNTING,0,1,"Unmounting the disposable save copy"))failed=1;
 if(f->unmount(c)){
  f->record(c,PM_SAVE_UNKNOWN,0,1,"Save mount state is unresolved; private staging retained");return -1;
 }
 if(failed)goto rejected;
 if(f->record(c,PM_SAVE_PUBLISHING,0,0,"Publishing the verified decrypted archive"))return -1;
 if(f->publish(c)){
  f->record(c,PM_SAVE_UNKNOWN,0,0,"Export publication did not finish; inspect its receipt before retrying");return -1;
 }
 if(f->record(c,PM_SAVE_COMPLETE,1,0,"Decrypted save exported and verified"))return -1;
 f->discard(c);return 0;
rejected:
 if(f->record(c,PM_SAVE_REJECTED,1,0,failure))return -1;
 f->discard(c);return -1;
}
