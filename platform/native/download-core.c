// SPDX-License-Identifier: GPL-3.0-or-later
#include "download-core.h"
#include <string.h>
#include <stdlib.h>
static int equal(const char *a,size_t n,const char *b){if(n!=strlen(b))return 0;for(size_t i=0;i<n;i++){unsigned char c=a[i];if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)b[i])return 0;}return 1;}
int pm_download_space(uint64_t block_size,uint64_t available,uint64_t needed,uint64_t *budget){
 const uint64_t reserve=16*UINT64_C(1024)*1024,window=4*UINT64_C(1024)*1024;
 if(!budget)return -1;*budget=0;
 if(!block_size||available>UINT64_MAX/block_size)return -1;
 uint64_t free_bytes=available*block_size;
 if(free_bytes<reserve||needed>free_bytes-reserve)return -1;
 *budget=free_bytes-reserve<window?free_bytes-reserve:window;return 0;
}
int pm_download_headers(const char *text,size_t size,PmDownloadHeaders *out){
 if(!text||!out||!size||size>32768)return -1;memset(out,0,sizeof *out);
 int location=0,encoding=0;size_t at=0;
 if(text[size-1]==0)size--;
 while(at<size){size_t end=at;while(end<size&&text[end]!='\n')end++;size_t n=end-at;if(n&&text[at+n-1]=='\r')n--;
  const char *line=text+at;at=end<size?end+1:end;if(!n)continue;
  for(size_t i=0;i<n;i++)if(((unsigned char)line[i]<32&&line[i]!='\t')||(unsigned char)line[i]==127)return -1;
  if(n>=5&&!memcmp(line,"HTTP/",5))continue;
  const char *colon=memchr(line,':',n);if(!colon||colon==line)return -1;
  size_t key=(size_t)(colon-line),length=n-key-1;const char *value=colon+1;
  while(length&&(*value==' '||*value=='\t')){value++;length--;}
  while(length&&(value[length-1]==' '||value[length-1]=='\t'))length--;
  if(equal(line,key,"content-length")){
   if(out->has_length||!length||length>20)return -1;uint64_t v=0;
   for(size_t i=0;i<length;i++){if(value[i]<'0'||value[i]>'9'||v>(UINT64_MAX-(unsigned)(value[i]-'0'))/10)return -1;v=v*10+(unsigned)(value[i]-'0');}
   out->length=v;out->has_length=1;
  }else if(equal(line,key,"transfer-encoding")){
   if(out->chunked||!equal(value,length,"chunked"))return -1;out->chunked=1;
  }else if(equal(line,key,"content-encoding")){
   if(encoding++||!equal(value,length,"identity"))return -1;
  }else if(equal(line,key,"location")){
   if(location++||!length||length>=sizeof out->location)return -1;
   for(size_t i=0;i<length;i++)if((unsigned char)value[i]<=32||(unsigned char)value[i]>=127||value[i]=='\\')return -1;
   memcpy(out->location,value,length);out->location[length]=0;
  }
 }
 return out->has_length&&out->chunked?-1:0;
}
int pm_download_chunk(const PmDownloadStream *io,const PmDownloadHeaders *h,uint64_t limit,uint64_t *total,const void *data,size_t size){
 if(io->cancelled(io->context))return PM_DL_CANCELLED;
 if(*total>limit||size>limit-*total)return PM_DL_LIMIT;
 if(h->has_length&&(*total>h->length||size>h->length-*total))return PM_DL_TRUNCATED;
 if(io->write(io->context,data,size))return PM_DL_WRITE;
 *total+=size;io->progress(io->context,*total,h->has_length?h->length:0);return PM_DL_OK;
}
int pm_download_complete(const PmDownloadHeaders *h,uint64_t total){
 if(!total)return PM_DL_EMPTY;
 return h->has_length&&total!=h->length?PM_DL_TRUNCATED:PM_DL_OK;
}
int pm_download_stream(const PmDownloadStream *io,const PmDownloadHeaders *h,uint64_t limit){
 if(h->has_length&&h->length>limit)return PM_DL_LIMIT;
 unsigned char buffer[65536];uint64_t total=0;
 for(;;){
  if(io->cancelled(io->context))return PM_DL_CANCELLED;
  int got=io->read(io->context,buffer,sizeof buffer);if(got<0)return PM_DL_READ;if(!got)break;
  if((size_t)got>sizeof buffer)return PM_DL_LIMIT;
  int result=pm_download_chunk(io,h,limit,&total,buffer,(size_t)got);if(result)return result;
 }
 return pm_download_complete(h,total);
}
