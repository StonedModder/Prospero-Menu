// SPDX-License-Identifier: GPL-3.0-or-later
#include "user-read.h"
#include <string.h>
int pm_user_read(const PmUserReader *r,uint64_t address,void *out,size_t bytes){
 if(!r||!r->current||!r->translate||!r->physical||!out||!bytes||bytes>65536)return -1;
 if(!address||address>=UINT64_C(0x800000000000)||bytes>UINT64_C(0x800000000000)-address)goto failed;
 for(size_t done=0;done<bytes;){
  uint64_t physical=0,available=0,at=address+done;
  if(!r->current(r->context)||r->translate(r->context,at,&physical,&available)||!available||physical>=UINT64_C(0x10000000000000))goto failed;
  size_t chunk=4096-(at&4095);if(chunk>bytes-done)chunk=bytes-done;if(available<chunk)chunk=(size_t)available;
  if(chunk>UINT64_C(0x10000000000000)-physical||r->physical(r->context,physical,(unsigned char*)out+done,chunk)||!r->current(r->context))goto failed;
  done+=chunk;
 }
 return 0;
failed:memset(out,0,bytes);return -1;
}
