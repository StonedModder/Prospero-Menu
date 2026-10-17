// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded HTTP(S) staging only. Redirect policy and explicit execution are in downloads.mjs.
#include "quickjs.h"
#include "download-core.h"
#include "download-ca.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifndef PROSPEROMENU_DOWNLOAD_ROOT
#define PROSPEROMENU_DOWNLOAD_ROOT "/data/ProsperoMenu/downloads"
#endif
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct {char request[33],url[8193],location[8193],message[192];uint64_t limit,done,total,deadline;int timeout_seconds,phase,cancel,code,status;} job;
// Terminal publication occurs only after closing all handles:
// 1 running, 2 downloaded, 3 redirect, 4 failed, 5 cancelled.
static pthread_once_t curl_once=PTHREAD_ONCE_INIT;
static CURLcode curl_init_result=CURLE_FAILED_INIT;
static void initialize_curl(void){curl_init_result=curl_global_init(CURL_GLOBAL_DEFAULT);}
static uint64_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int cancelled(void *context){(void)context;pthread_mutex_lock(&mutex);int value=job.cancel||now_ms()>job.deadline;pthread_mutex_unlock(&mutex);return value;}
static void progress(void *context,uint64_t done,uint64_t total){(void)context;pthread_mutex_lock(&mutex);job.done=done;job.total=total;pthread_mutex_unlock(&mutex);}
typedef struct {
 int fd,dir,owned,headers_ready,header_stop,status,error;
 size_t header_size,all_headers;uint64_t done,space_budget;
 char file[48],header[32769];PmDownloadHeaders parsed;const char *message;
} Transfer;
static int redirect_status(int s){return s==301||s==302||s==303||s==307||s==308;}
static int check_space(Transfer *t,uint64_t needed){
 struct statvfs capacity;
 if(fstatvfs(t->dir,&capacity)||pm_download_space(capacity.f_frsize?capacity.f_frsize:capacity.f_bsize,capacity.f_bavail,needed,&t->space_budget)){
  t->message="Not enough free space to continue the download. Free space in console storage and try again.";return -1;
 }
 return 0;
}
static int write_data(void *context,const void *data,size_t size){
 Transfer *t=context;size_t at=0;
 if(size>t->space_budget&&check_space(t,size))return -1;
 while(at<size){ssize_t n=write(t->fd,(const char*)data+at,size-at);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;at+=(size_t)n;}
 t->space_budget=size>t->space_budget?0:t->space_budget-size;return 0;
}
static int open_output(Transfer *t){
 if(mkdir(PROSPEROMENU_DOWNLOAD_ROOT,0700)&&errno!=EEXIST){t->message="Could not create the download folder.";return -1;}
 t->dir=open(PROSPEROMENU_DOWNLOAD_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
 if(t->dir<0){t->message="The download folder is unavailable.";return -1;}
 // The size cap is a bound, never an estimate of an unknown response's size.
 // Known files still fail early; all transfers recheck real space as they grow.
 if(check_space(t,t->parsed.has_length?t->parsed.length:1))return -1;
 t->fd=openat(t->dir,t->file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
 if(t->fd<0){t->message="A file already occupies this download slot. The existing file was kept.";return -1;}t->owned=1;return 0;
}
static size_t receive_header(char *data,size_t size,size_t count,void *context){
 Transfer *t=context;if(size&&count>SIZE_MAX/size)return 0;size_t bytes=size*count;
 if(cancelled(NULL)||bytes>65536-t->all_headers)return 0;t->all_headers+=bytes;
 if(bytes>=5&&!memcmp(data,"HTTP/",5)){
  // curl callbacks are length-delimited, not NUL-terminated.
  char line[96];size_t n=bytes<sizeof line-1?bytes:sizeof line-1;memcpy(line,data,n);line[n]=0;
  int status=0;if(t->headers_ready||sscanf(line,"HTTP/%*s %3d",&status)!=1||status<100||status>599){t->message="The server returned an invalid HTTP status.";return 0;}
  t->status=status;t->header_size=0;
 }
 if(bytes>32768-t->header_size){t->message="The server returned too many download headers.";return 0;}
 memcpy(t->header+t->header_size,data,bytes);t->header_size+=bytes;t->header[t->header_size]=0;
 if((bytes==2&&!memcmp(data,"\r\n",2))||(bytes==1&&data[0]=='\n')){
  if(t->status>=100&&t->status<200){t->header_size=0;return bytes;}
  if(pm_download_headers(t->header,t->header_size,&t->parsed)){t->message="The server returned invalid or unsupported download headers.";return 0;}
  t->headers_ready=1;
  if(t->status!=200){t->header_stop=1;return 0;}
  if(t->parsed.has_length&&t->parsed.length>job.limit){t->error=PM_DL_LIMIT;return 0;}
  progress(NULL,0,t->parsed.has_length?t->parsed.length:0);
 }
 return bytes;
}
static size_t receive_body(char *data,size_t size,size_t count,void *context){
 Transfer *t=context;if(size&&count>SIZE_MAX/size)return 0;size_t bytes=size*count;
 if(!t->headers_ready||t->status!=200){t->message="The server did not return a downloadable file.";return 0;}
 if(t->fd<0&&open_output(t))return 0;
 PmDownloadStream io={t,cancelled,NULL,write_data,progress};t->error=pm_download_chunk(&io,&t->parsed,job.limit,&t->done,data,bytes);return t->error?0:bytes;
}
static int curl_progress(void *context,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)context;(void)a;(void)b;(void)c;(void)d;return cancelled(NULL);}
static void *worker(void *unused){
 (void)unused;Transfer *t=calloc(1,sizeof *t);CURL *curl=NULL;char *ca=NULL;int phase=4,code=0;
 const char *message="Could not initialize the console download service.";
 if(!t)goto publish;t->fd=-1;t->dir=-1;snprintf(t->file,sizeof t->file,"%s.download",job.request);
 pthread_once(&curl_once,initialize_curl);if(curl_init_result!=CURLE_OK){code=curl_init_result;goto cleanup;}
 curl=curl_easy_init();if(!curl)goto cleanup;
 size_t ca_bytes=0;for(size_t i=0;i<sizeof pm_ca_pem/sizeof pm_ca_pem[0];i++)ca_bytes+=strlen(pm_ca_pem[i]);
 ca=malloc(ca_bytes+1);if(!ca)goto cleanup;size_t at=0;
 for(size_t i=0;i<sizeof pm_ca_pem/sizeof pm_ca_pem[0];i++){size_t n=strlen(pm_ca_pem[i]);memcpy(ca+at,pm_ca_pem[i],n);at+=n;}ca[at]=0;
 struct curl_blob trust={ca,ca_bytes,CURL_BLOB_COPY};
#define OPTION(key,value) do{code=curl_easy_setopt(curl,key,value);if(code)goto cleanup;}while(0)
 OPTION(CURLOPT_CAINFO_BLOB,&trust);OPTION(CURLOPT_SSL_VERIFYPEER,1L);OPTION(CURLOPT_SSL_VERIFYHOST,2L);
 OPTION(CURLOPT_URL,job.url);OPTION(CURLOPT_PROTOCOLS_STR,"http,https");OPTION(CURLOPT_FOLLOWLOCATION,0L);
 OPTION(CURLOPT_PROXY,"");OPTION(CURLOPT_NETRC,CURL_NETRC_IGNORED);
 OPTION(CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);OPTION(CURLOPT_USERAGENT,"ProsperoMenu/0.1");
 OPTION(CURLOPT_NOSIGNAL,1L);OPTION(CURLOPT_CONNECTTIMEOUT,30L);OPTION(CURLOPT_TIMEOUT,(long)job.timeout_seconds);
 OPTION(CURLOPT_LOW_SPEED_LIMIT,1L);OPTION(CURLOPT_LOW_SPEED_TIME,30L);
 OPTION(CURLOPT_HEADERFUNCTION,receive_header);OPTION(CURLOPT_HEADERDATA,t);OPTION(CURLOPT_WRITEFUNCTION,receive_body);OPTION(CURLOPT_WRITEDATA,t);
 OPTION(CURLOPT_XFERINFOFUNCTION,curl_progress);OPTION(CURLOPT_XFERINFODATA,t);OPTION(CURLOPT_NOPROGRESS,0L);
#undef OPTION
 code=curl_easy_perform(curl);
 if(cancelled(NULL)){phase=5;message="Download cancelled.";goto cleanup;}
 if(t->message){message=t->message;goto cleanup;}
 if(t->error){
  const char *errors[]={"", "Download cancelled.", "This download exceeds the file-size limit.", "The server sent a different file size.", "The download connection was interrupted.", "Could not save the downloaded file.", "The server returned an empty file."};
  message=errors[t->error];if(t->error==PM_DL_CANCELLED)phase=5;goto cleanup;
 }
 if(t->header_stop&&t->headers_ready){
  if(redirect_status(t->status)&&t->parsed.location[0]){phase=3;message="Following download link.";code=0;}else message="The download server did not return a file.";goto cleanup;
 }
 if(code){message=(code==CURLE_PEER_FAILED_VERIFICATION||code==CURLE_SSL_CACERT_BADFILE)?"HTTPS verification failed. Check the link and console date.":"The download connection failed or timed out. Check the link and network.";goto cleanup;}
 if(!t->headers_ready||t->status!=200){message="The server did not return a downloadable file.";goto cleanup;}
 code=pm_download_complete(&t->parsed,t->done);
 if(code){message=code==PM_DL_EMPTY?"The server returned an empty file.":"The server ended the download before the file was complete.";goto cleanup;}
 if(fsync(t->fd)){message="Could not finish saving the download.";goto cleanup;}
 if(close(t->fd)){t->fd=-1;message="Could not close the downloaded file.";goto cleanup;}t->fd=-1;
 if(fsync(t->dir)){message="Could not save the download directory.";goto cleanup;}
 phase=2;message="Download complete. Checking the file.";
cleanup:
 if(curl)curl_easy_cleanup(curl);free(ca);
 if(t->fd>=0)close(t->fd);
 if(t->dir>=0){if(t->owned&&phase!=2)unlinkat(t->dir,t->file,0);close(t->dir);}
publish:
 pthread_mutex_lock(&mutex);job.code=code;job.status=t?t->status:0;job.phase=phase;
 snprintf(job.message,sizeof job.message,"%s",message);if(phase==3)snprintf(job.location,sizeof job.location,"%s",t->parsed.location);
 pthread_mutex_unlock(&mutex);free(t);return NULL;
}
static int valid_id(const char *id){if(!id||strlen(id)!=32)return 0;for(int i=0;i<32;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return 0;return 1;}
static int valid_url(const char *url){
 if(!url||strlen(url)>8192)return 0;int offset=!strncmp(url,"https://",8)?8:!strncmp(url,"http://",7)?7:0;if(!offset||!url[offset])return 0;
 int authority=1;for(size_t i=(size_t)offset;url[i];i++){unsigned char c=url[i];if(c<=32||c>=127||c=='\\'||c=='#')return 0;if(c=='/'||c=='?')authority=0;if(authority&&(c=='@'||c=='%'))return 0;}return 1;
}
static JSValue binding(JSContext *c,JSValueConst self,int argc,JSValueConst *args,int operation){
 (void)self;if(argc<(operation==0?3:1))return JS_ThrowTypeError(c,"Expected download request");
 const char *id=JS_ToCString(c,args[0]);if(!valid_id(id)){if(id)JS_FreeCString(c,id);return JS_ThrowTypeError(c,"Invalid download request");}
 JSValue result=JS_UNDEFINED;const char *url=NULL;int64_t limit=0;pthread_mutex_lock(&mutex);
 if(operation==0){
  int32_t timeout=900;url=JS_ToCString(c,args[1]);JS_ToInt64(c,&limit,args[2]);if(argc>3)JS_ToInt32(c,&timeout,args[3]);
  if(job.request[0]||!valid_url(url)||limit<64||limit>INT64_C(1099511627776)||timeout<1||timeout>48*60*60)result=JS_ThrowInternalError(c,"Download service busy or invalid request");
  else {memset(&job,0,sizeof job);snprintf(job.request,sizeof job.request,"%s",id);snprintf(job.url,sizeof job.url,"%s",url);job.limit=(uint64_t)limit;job.timeout_seconds=timeout;job.deadline=now_ms()+(uint64_t)timeout*1000;job.phase=1;
   pthread_t thread;int rc=pthread_create(&thread,NULL,worker,NULL);if(rc){memset(&job,0,sizeof job);result=JS_ThrowInternalError(c,"Could not start download worker");}else pthread_detach(thread);
  }
 }else if(strcmp(job.request,id)){result=operation==1?JS_NULL:JS_ThrowInternalError(c,"Download request no longer active");}
 else if(operation==2)job.cancel=1;
 else if(operation==3){
  if(job.phase==1)result=JS_ThrowInternalError(c,"Download worker is still stopping");
  else{if(job.phase==2){char file[256];snprintf(file,sizeof file,PROSPEROMENU_DOWNLOAD_ROOT "/%s.download",id);if(unlink(file)&&errno!=ENOENT){result=JS_ThrowInternalError(c,"Could not remove staged download");goto done;}}memset(&job,0,sizeof job);}
 }else{
  result=JS_NewObject(c);
#define STR(name,value) JS_SetPropertyStr(c,result,name,JS_NewString(c,value))
#define NUM(name,value) JS_SetPropertyStr(c,result,name,JS_NewInt64(c,value))
  STR("request",job.request);STR("message",job.message);STR("location",job.location);NUM("phase",job.phase);NUM("code",job.code);NUM("status",job.status);NUM("bytes",job.done);NUM("total",job.total);
#undef STR
#undef NUM
 }
done:pthread_mutex_unlock(&mutex);if(url)JS_FreeCString(c,url);JS_FreeCString(c,id);return result;
}
void pm_download_bind(JSContext *c,JSValue api){const char *names[]={"downloadStart","downloadPoll","downloadCancel","downloadRelease"};for(int i=0;i<4;i++)JS_SetPropertyStr(c,api,names[i],JS_NewCFunctionMagic(c,binding,names[i],i==0?3:1,JS_CFUNC_generic_magic,i));}
