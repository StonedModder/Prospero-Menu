// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher never retries native dispatch in one boot. The selected runtime
// must independently confirm its complete configured startup before promotion.
#include "update-io.h"
#include "elfldr.h"
#include "kstuff-probe.h"
#include <ps5/kernel.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const unsigned char prosperomenu_fallback_start[],prosperomenu_fallback_end[];
static void notify(const char *text){extern int sceKernelSendNotificationRequest(int,const void*,size_t,int);unsigned char message[0xc30]={0};int target=-1;memcpy(message+0x10,&target,sizeof target);message[0x2c]=1;snprintf((char*)message+0x2d,1024,"%s",text);snprintf((char*)message+0x42d,1024,"cxml://psnotification/tex_icon_system");sceKernelSendNotificationRequest(0,message,sizeof message,0);}
int main(void){
  int lock=pm_update_lock();if(lock<0){notify("ProsperoMenu launcher: update selection is busy or unavailable.");return 1;}
  int server=open("/data/ProsperoMenu/server.lock",O_CREAT|O_RDWR|O_NOFOLLOW,0600);struct stat st;
  if(server<0||fstat(server,&st)||!S_ISREG(st.st_mode)||flock(server,LOCK_EX|LOCK_NB)){if(server>=0)close(server);close(lock);notify("ProsperoMenu is already running, or its workspace is unavailable.");return 1;}
  // The durable per-boot attempt below closes the interval between releasing
  // this lock and the child acquiring it. Never carry a locked descriptor into spawn.
  close(server);
  PmUpdateState state;int recovered=0;int64_t sec,usec;uint32_t fw=kernel_get_fw_version();
  PmUpdateHeader fallback;size_t fallback_size=(size_t)(prosperomenu_fallback_end-prosperomenu_fallback_start);
  if(pm_update_read(&state,&recovered)||recovered||pm_update_boot(&sec,&usec)||
     pm_update_save_image(prosperomenu_fallback_start,fallback_size,fw,&fallback)){
    close(lock);notify("ProsperoMenu launcher: saved selection or bundled fallback could not be verified. Nothing launched.");return 1;
  }
#ifdef PROSPEROMENU_BUNDLED_RUNTIME
  int planned=pm_update_plan_bundled(&state,sec,usec,&fallback);
  if(planned==-2){close(lock);notify("ProsperoMenu bundled test: a different update is pending. Discard or finish that update first.");return 1;}
#else
  int planned=pm_update_plan(&state,sec,usec,&fallback);
#endif
  if(planned){close(lock);notify("ProsperoMenu already has a launch attempt in this boot. Restart before another attempt.");return 1;}
  uint8_t *image=NULL;size_t size=0;
  if(pm_update_read_image(&state.attempt,fw,&image,&size)){
#ifdef PROSPEROMENU_BUNDLED_RUNTIME
    close(lock);notify("ProsperoMenu bundled test: embedded build could not be read. Nothing launched.");return 1;
#else
    PmUpdateHeader good=state.good;
    if(pm_update_header(&good,sizeof good,&good)||pm_update_read_image(&good,fw,&image,&size)){
      good=fallback;if(pm_update_read_image(&good,fw,&image,&size)){close(lock);notify("ProsperoMenu launcher: no verified fallback image is readable.");return 1;}
    }
    if(pm_update_fallback(&state,&good)){free(image);close(lock);return 1;}
#endif
  }
  const SnipersFirmwareProfile *profile=snipers_firmware_profile(fw);uint64_t base=KERNEL_ADDRESS_DATA_BASE;
  PmKstuffSnapshot first=pm_kstuff_read(base,profile),second=pm_kstuff_read(base,profile);
  if(!pm_kstuff_snapshot_equal(&first,&second)||!pm_kstuff_injection_ready(base,&second)){free(image);close(lock);notify("ProsperoMenu launcher: kernel state is not ready for payload dispatch.");return 1;}
  if(pm_update_write(&state)){free(image);close(lock);notify("ProsperoMenu launcher: could not save the launch intent. Nothing launched.");return 1;}
#ifdef PROSPEROMENU_BUNDLED_RUNTIME
  char source[65],message[128];pm_update_hex(state.attempt.source_hash,source);
  snprintf(message,sizeof message,"ProsperoMenu loading bundled build %.12s",source);notify(message);
#endif
  int pid=elfldr_spawn("/data/ProsperoMenu",-1,image+sizeof(PmUpdateHeader),"ProsperoMenu",size-sizeof(PmUpdateHeader));free(image);
  if(pid<=0){close(lock);notify("ProsperoMenu launch was not confirmed. Restart before trying again.");return 1;}
  state.pid=pid;int saved=pm_update_write(&state);close(lock);
  if(saved){notify("ProsperoMenu launched, but its update receipt could not be saved. Update confirmation is held.");return 1;}
  if(state.attempt_kind==PM_BOOT_ROLLBACK)notify("ProsperoMenu recovered to a previous build. Review Updates on your phone.");
  return 0;
}
