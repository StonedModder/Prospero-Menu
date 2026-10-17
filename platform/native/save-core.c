// SPDX-License-Identifier: GPL-3.0-or-later
#include "save-core.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>
int pm_save_hex(const char* s,size_t n){if(!s||strlen(s)!=n)return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
int pm_save_user(const char* s){size_t n=s?strlen(s):0;return n&&n<=8&&pm_save_hex(s,n);}
int pm_save_platform(const char* s){
 if(!s||strlen(s)!=9)return 0;for(unsigned i=4;i<9;i++)if(s[i]<'0'||s[i]>'9')return 0;
 if(!memcmp(s,"PPSA",4)||!memcmp(s,"PPSB",4)||!memcmp(s,"MOUU",4))return 5;
 const char* ps4[]={"CUSA","PCAS","PCJS","PCKS","CUHJ","SCUS"};
 for(unsigned i=0;i<sizeof ps4/sizeof ps4[0];i++)if(!memcmp(s,ps4[i],4))return 4;return 0;
}
int pm_save_slot(const char* s){
 size_t n=s?strlen(s):0;if(n<7||n>127||memcmp(s,"sdimg_",6)||!strncmp(s,"sdimg_sce_bu_",13))return 0;
 for(size_t i=6;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||s[i]=='.'))return 0;
 return strcmp(s+6,".")&&strcmp(s+6,"..");
}
int pm_save_relative(const char* s){
 size_t n=s?strlen(s):0;if(!n||n>1023||s[0]=='/'||s[n-1]=='/')return 0;
 // PAX paths are UTF-8. Reject malformed/overlong sequences rather than
 // producing an archive whose extractor interprets a different filename.
 for(size_t i=0;i<n;){unsigned char c=(unsigned char)s[i++];if(c<128)continue;
  unsigned more;uint32_t code,min;
  if(c>=0xc2&&c<=0xdf){more=1;code=c&31;min=0x80;}
  else if(c>=0xe0&&c<=0xef){more=2;code=c&15;min=0x800;}
  else if(c>=0xf0&&c<=0xf4){more=3;code=c&7;min=0x10000;}else return 0;
  if(n-i<more)return 0;while(more--){c=(unsigned char)s[i++];if((c&0xc0)!=0x80)return 0;code=(code<<6)|(c&63);}
  if(code<min||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return 0;
 }
 const char* begin=s;for(const char* p=s;;p++){
  unsigned char c=(unsigned char)*p;if(c&&(c<32||c==127||c=='\\'))return 0;
  if(!c||c=='/'){size_t length=(size_t)(p-begin);if(!length||(length==1&&begin[0]=='.')||(length==2&&!memcmp(begin,"..",2)))return 0;begin=p+1;}
  if(!c)break;
 }return 1;
}
int pm_save_request_valid(const PmSaveRequest* r){
 if(!r||r->magic!=PM_SAVE_MAGIC||r->version!=1||r->reserved||r->request[32]||r->user[8]||r->title[9]||r->slot[127]||r->stamp[64]||r->volume[64]||
   !pm_save_hex(r->request,32)||r->bootSec<=0||r->bootUsec<0||r->bootUsec>=1000000||r->destination>8)return 0;
 if(r->mode==PM_SAVE_CATALOG)return !r->destination&&!r->platform&&!r->user[0]&&!r->title[0]&&!r->slot[0]&&!r->stamp[0]&&!r->volume[0];
 return r->mode==PM_SAVE_EXPORT&&pm_save_user(r->user)&&pm_save_platform(r->title)==(int)r->platform&&
  pm_save_slot(r->slot)&&pm_save_hex(r->stamp,64)&&(r->destination?pm_save_hex(r->volume,64):!r->volume[0]);
}
void pm_save_digest(const void* data,size_t n,unsigned char out[32]){SHA256_CTX h;sha256_init(&h);sha256_update(&h,data,n);sha256_final(&h,out);}
void pm_save_hash_text(const unsigned char digest[32],char text[65]){for(unsigned i=0;i<32;i++)snprintf(text+i*2,3,"%02x",digest[i]);}
static int zeros(PmSaveWrite write,void* ctx,size_t bytes){static const unsigned char zero[1024]={0};return write(ctx,zero,bytes);}
static int header(PmSaveWrite write,void* ctx,const char* path,uint64_t bytes,char type){
 unsigned char h[512]={0};if(strlen(path)>100||bytes>=UINT64_C(8589934592))return -1;
 memcpy(h,path,strlen(path));snprintf((char*)h+100,8,"%07o",type=='5'?0700:0600);
 memcpy(h+108,"0000000",7);memcpy(h+116,"0000000",7);snprintf((char*)h+124,12,"%011llo",(unsigned long long)bytes);
 memcpy(h+136,"00000000000",11);memset(h+148,' ',8);h[156]=(unsigned char)type;memcpy(h+257,"ustar",5);memcpy(h+263,"00",2);
 unsigned sum=0;for(unsigned i=0;i<512;i++)sum+=h[i];snprintf((char*)h+148,7,"%06o",sum);h[155]=' ';return write(ctx,h,sizeof h);
}
static int pax(char* out,size_t cap,const char* key,const char* value){
 size_t base=strlen(key)+strlen(value)+3,length=base+1;
 for(;;){char n[32];int digits=snprintf(n,sizeof n,"%zu",length);size_t next=base+(size_t)digits;if(next==length)break;length=next;}
 int n=snprintf(out,cap,"%zu %s=%s\n",length,key,value);return n<0||(size_t)n!=length||length>=cap?-1:n;
}
int pm_save_tar_header(PmSaveWrite write,void* ctx,const char* path,uint64_t bytes,int directory){
 if(!write||!pm_save_relative(path)||bytes>PM_SAVE_MAX_BYTES||(directory&&bytes))return -1;
 char extended[1200];int used=0,n;
 if(strlen(path)>100){n=pax(extended,sizeof extended,"path",path);if(n<0)return -1;used=n;}
 if(bytes>=UINT64_C(8589934592)){char value[32];snprintf(value,sizeof value,"%llu",(unsigned long long)bytes);n=pax(extended+used,sizeof extended-(size_t)used,"size",value);if(n<0)return -1;used+=n;}
 if(used&&(header(write,ctx,"PaxHeaders/entry",(uint64_t)used,'x')||write(ctx,extended,(size_t)used)||pm_save_tar_padding(write,ctx,(uint64_t)used)))return -1;
 return header(write,ctx,strlen(path)>100?"extended-entry":path,bytes>=UINT64_C(8589934592)?0:bytes,directory?'5':'0');
}
int pm_save_tar_padding(PmSaveWrite write,void* ctx,uint64_t bytes){size_t pad=(512-(size_t)(bytes%512))%512;return pad?zeros(write,ctx,pad):0;}
int pm_save_tar_end(PmSaveWrite write,void* ctx){return zeros(write,ctx,1024);}
int pm_save_json_string(PmSaveWrite write,void* ctx,const char* text){
 if(write(ctx,"\"",1))return -1;for(const unsigned char* p=(const unsigned char*)text;*p;p++){
  char escaped[7];if(*p=='"'||*p=='\\'){escaped[0]='\\';escaped[1]=(char)*p;if(write(ctx,escaped,2))return -1;}
  else if(*p<32||*p==127){snprintf(escaped,sizeof escaped,"\\u%04x",*p);if(write(ctx,escaped,6))return -1;}
  else if(write(ctx,p,1))return -1;
 }return write(ctx,"\"",1);
}
