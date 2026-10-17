// SPDX-License-Identifier: GPL-3.0-or-later
#include "user-write.h"
int pm_user_write(const PmUserWriter *w,uint64_t base,size_t span,uint64_t at,const void *bytes,size_t size){
 if(!w||!w->current||!w->translate||!w->physical||!bytes||!size||size>65536||
  !base||base>=UINT64_C(0x800000000000)||!span||span>65536||span>UINT64_C(0x800000000000)-base||
  at<base||at-base>=span||size>span-(at-base))return -1;
 for(size_t done=0;done<size;){
  uint64_t address=at+done,physical=0,available=0;
  if(!w->current(w->context)||w->translate(w->context,address,&physical,&available)||!available||physical>=UINT64_C(0x10000000000000))return -1;
  size_t chunk=4096-(address&4095);if(chunk>size-done)chunk=size-done;if(available<chunk)chunk=(size_t)available;
  if(chunk>UINT64_C(0x10000000000000)-physical||w->physical(w->context,physical,(const unsigned char*)bytes+done,chunk)||!w->current(w->context))return -1;
  done+=chunk;
 }
 return 0;
}
