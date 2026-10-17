// SPDX-License-Identifier: GPL-3.0-or-later
#include "package-http.h"
#include "package-core.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
static int equal(const char* a,size_t n,const char* b){
 if(n!=strlen(b))return 0;for(size_t i=0;i<n;i++){unsigned char c=a[i];if(c>='A'&&c<='Z')c+=32;if(c!=(unsigned char)b[i])return 0;}return 1;
}
static int number(const char** p,const char* end,uint64_t* out){
 const char* first=*p;uint64_t n=0;
 while(*p<end&&**p>='0'&&**p<='9'){unsigned d=(unsigned)(*(*p)++-'0');if(n>(UINT64_MAX-d)/10)return -1;n=n*10+d;}
 if(*p==first)return -1;*out=n;return 0;
}
int pm_package_http_parse(const char* h,size_t bytes,const char* target,uint64_t size,PmPackageHttp* r){
 if(!r)return -1;*r=(PmPackageHttp){.status=400};
 if(!h||!target||!size||size>PM_PACKAGE_MAX||bytes<4||bytes>4096||memcmp(h+bytes-4,"\r\n\r\n",4))return -1;
 for(size_t i=0;i<bytes;i++)if(!h[i]||((unsigned char)h[i]<32&&h[i]!='\r'&&h[i]!='\n'&&h[i]!='\t')||(unsigned char)h[i]==127)return -1;
 const char* end=h+bytes;const char* line=memchr(h,'\r',bytes);if(!line||line+1>=end||line[1]!='\n')return -1;
 const char* first=memchr(h,' ',(size_t)(line-h));if(!first)return -1;
 if(first-h==3&&!memcmp(h,"GET",3))r->head=0;
 else if(first-h==4&&!memcmp(h,"HEAD",4))r->head=1;
 else {r->status=405;return 0;}
 const char* second=memchr(first+1,' ',(size_t)(line-first-1));if(!second)return -1;
 if(line-second!=9||(memcmp(second+1,"HTTP/1.1",8)&&memcmp(second+1,"HTTP/1.0",8)))return -1;
 // PlayGo appends product/serverIpAddr/r query parameters to the supplied URL.
 // Match the exact pathname; queries never select or open another file.
 const char* query=memchr(first+1,'?',(size_t)(second-first-1));
 size_t pathname=(size_t)((query?query:second)-first-1);
 if(pathname!=strlen(target)||memcmp(first+1,target,pathname)){r->status=404;return 0;}
 unsigned range=0,length=0,host=0;const char* rangeStart=NULL;const char* rangeEnd=NULL;
 const char* p=line+2;
 while(p<end-2){
  line=memchr(p,'\r',(size_t)(end-p));if(!line||line+1>=end||line[1]!='\n'||p==line||*p==' '||*p=='\t')return -1;
  const char* colon=memchr(p,':',(size_t)(line-p));if(!colon||colon==p)return -1;
  for(const char* q=p;q<colon;q++)if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='-'))return -1;
  const char* value=colon+1;while(value<line&&(*value==' '||*value=='\t'))value++;
  const char* tail=line;while(tail>value&&(tail[-1]==' '||tail[-1]=='\t'))tail--;
  for(const char* q=value;q<tail;q++)if(*q=='\r'||*q=='\n')return -1;
  size_t name=(size_t)(colon-p);
  if(equal(p,name,"range")){if(range++)return -1;rangeStart=value;rangeEnd=tail;}
  else if(equal(p,name,"content-length")){uint64_t n=0;if(length++||number(&value,tail,&n)||value!=tail||n)return -1;}
  else if(equal(p,name,"transfer-encoding")||equal(p,name,"expect"))return -1;
  else if(equal(p,name,"host")){if(host++||value==tail)return -1;}
  p=line+2;
 }
 if(p!=end-2)return -1;
 r->status=200;r->start=0;r->length=size;
 if(range&&!r->head){
  p=rangeStart;end=rangeEnd;uint64_t start=0,last=0;
  r->status=416;r->length=0;
  if(end-p<7||memcmp(p,"bytes=",6))return 0;p+=6;
  if(*p=='-'){p++;if(number(&p,end,&last)||p!=end||!last)return 0;start=last>=size?0:size-last;last=size-1;}
  else {if(number(&p,end,&start)||p==end||*p++!='-'||start>=size)return 0;
   if(p==end)last=size-1;else if(number(&p,end,&last)||p!=end||last<start)return 0;if(last>=size)last=size-1;}
  r->status=206;r->start=start;r->length=last-start+1;
 }
 return 0;
}
int pm_package_http_response(char* buffer,size_t capacity,const PmPackageHttp* r,uint64_t size){
 if(!buffer||!r||!capacity)return -1;
 const char* reason=r->status==200?"OK":r->status==206?"Partial Content":r->status==404?"Not Found":r->status==405?"Method Not Allowed":r->status==416?"Range Not Satisfiable":"Bad Request";
 char extra[160]="";int n;
 if(r->status==206){if(!r->length||r->start>=size||r->length>size-r->start)return -1;
  snprintf(extra,sizeof extra,"Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n",r->start,r->start+r->length-1,size);}
 else if(r->status==416)snprintf(extra,sizeof extra,"Content-Range: bytes */%" PRIu64 "\r\n",size);
 else if(r->status==405)snprintf(extra,sizeof extra,"Allow: GET, HEAD\r\n");
 uint64_t length=r->status==200||r->status==206?r->length:0;
 n=snprintf(buffer,capacity,"HTTP/1.1 %u %s\r\nContent-Length: %" PRIu64 "\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nConnection: close\r\nCache-Control: no-store\r\n%s\r\n",r->status,reason,length,extra);
 return n<0||(size_t)n>=capacity?-1:n;
}
