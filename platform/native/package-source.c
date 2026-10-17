// SPDX-License-Identifier: GPL-3.0-or-later
// Original bounded loopback source. No pathname/URL supplied by an HTTP client
// is ever opened; all reads use the already verified, duplicated package fd.
#include "package-http.h"
#include "package-core.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <io.h>
#include <sys/stat.h>
typedef SOCKET Socket;
typedef struct _stat64 FileStat;
#define INVALID INVALID_SOCKET
#define close_socket closesocket
#define file_stat _fstat64
#define file_close _close
#define file_dup _dup
#define Poll WSAPoll
typedef WSAPOLLFD PollFd;
static uint64_t millis(void){return GetTickCount64();}
static int nonblock(Socket s){u_long n=1;return ioctlsocket(s,FIONBIO,&n);}
static int retry(void){int e=WSAGetLastError();return e==WSAEWOULDBLOCK||e==WSAEINTR;}
static int read_file(int fd,uint64_t at,void* data,size_t n){if(_lseeki64(fd,(__int64)at,SEEK_SET)<0)return -1;return _read(fd,data,(unsigned)n);}
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
typedef int Socket;
typedef struct stat FileStat;
#define INVALID (-1)
#define close_socket close
#define file_stat fstat
#define file_close close
#define file_dup dup
#define Poll poll
typedef struct pollfd PollFd;
static uint64_t millis(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int nonblock(Socket s){int flags=fcntl(s,F_GETFL,0);return flags<0?-1:fcntl(s,F_SETFL,flags|O_NONBLOCK);}
static int retry(void){return errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK;}
static int read_file(int fd,uint64_t at,void* data,size_t n){return (int)pread(fd,data,n,(off_t)at);}
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define CLIENTS 4
typedef struct {
 Socket socket;unsigned ready,head;size_t received,pending,sent;uint64_t at,left,deadline;
 char header[4097];unsigned char buffer[32768];
} Client;
struct PmPackageSource {
 Socket listener;int file;FileStat original;uint64_t bytes;char target[64];
 atomic_int stop,alive,fault;Client clients[CLIENTS];
#ifdef _WIN32
 HANDLE thread;
#else
 pthread_t thread;
#endif
};
static void drop(Client* c){if(c->socket!=INVALID)close_socket(c->socket);memset(c,0,sizeof *c);c->socket=INVALID;}
static int same_file(PmPackageSource* s){
 FileStat st;if(file_stat(s->file,&st)||st.st_size<0||(uint64_t)st.st_size!=s->bytes||st.st_dev!=s->original.st_dev||st.st_ino!=s->original.st_ino)return 0;
#ifdef _WIN32
 return st.st_mtime==s->original.st_mtime;
#else
 return st.st_mtim.tv_sec==s->original.st_mtim.tv_sec&&st.st_mtim.tv_nsec==s->original.st_mtim.tv_nsec;
#endif
}
static int respond(PmPackageSource* s,Client* c,size_t bytes){
 PmPackageHttp r;
 if(pm_package_http_parse(c->header,bytes,s->target,s->bytes,&r))r=(PmPackageHttp){.status=400};
 int n=pm_package_http_response((char*)c->buffer,sizeof c->buffer,&r,s->bytes);if(n<0)return -1;
 c->pending=(size_t)n;c->sent=0;c->at=r.start;c->left=(r.status==200||r.status==206)&&!r.head?r.length:0;c->ready=1;c->head=r.head;
 return 0;
}
static int receive_header(PmPackageSource* s,Client* c,uint64_t now){
 int n=recv(c->socket,c->header+c->received,(int)(4096-c->received),0);
 if(n<0)return retry()?0:-1;if(!n)return -1;c->received+=(size_t)n;c->header[c->received]=0;
 char* end=strstr(c->header,"\r\n\r\n");
 if(end){size_t bytes=(size_t)(end-c->header)+4;if(bytes!=c->received)return -1;if(respond(s,c,bytes))return -1;c->deadline=now+15000;}
 else if(c->received==4096)return -1;
 return 0;
}
static int send_part(PmPackageSource* s,Client* c,uint64_t now){
 if(!same_file(s)){atomic_store(&s->fault,1);return -1;}
 if(c->sent==c->pending){
  if(!c->left)return -1;size_t count=c->left>sizeof c->buffer?sizeof c->buffer:(size_t)c->left;
  int n=read_file(s->file,c->at,c->buffer,count);
  if(n<0&&errno==EINTR)return 0;if(n<=0||(size_t)n>count||!same_file(s)){atomic_store(&s->fault,1);return -1;}
  c->at+=(unsigned)n;c->left-=(unsigned)n;c->pending=(unsigned)n;c->sent=0;
 }
 int n=send(c->socket,(const char*)c->buffer+c->sent,(int)(c->pending-c->sent),MSG_NOSIGNAL);
 if(n<0)return retry()?0:-1;if(!n)return -1;c->sent+=(unsigned)n;c->deadline=now+15000;
 return 0;
}
static void serve(PmPackageSource* s){
 while(!atomic_load(&s->stop)&&!atomic_load(&s->fault)){
  PollFd fds[CLIENTS+1];memset(fds,0,sizeof fds);fds[0].fd=s->listener;fds[0].events=POLLIN;
  for(unsigned i=0;i<CLIENTS;i++){fds[i+1].fd=s->clients[i].socket;fds[i+1].events=s->clients[i].ready?POLLOUT:POLLIN;}
  int n=Poll(fds,CLIENTS+1,100);if(n<0){if(retry())continue;atomic_store(&s->fault,1);break;}
  uint64_t now=millis();if(!now){atomic_store(&s->fault,1);break;}
  if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL)){atomic_store(&s->fault,1);break;}
  if(fds[0].revents&POLLIN){
   Socket peer=accept(s->listener,NULL,NULL);
   if(peer!=INVALID){unsigned i=0;for(;i<CLIENTS;i++)if(s->clients[i].socket==INVALID)break;
    if(i==CLIENTS||nonblock(peer))close_socket(peer);else {s->clients[i].socket=peer;s->clients[i].deadline=now+5000;fds[i+1].revents=0;}}
   else if(!retry()){atomic_store(&s->fault,1);break;}
  }
  for(unsigned i=0;i<CLIENTS;i++){
   Client* c=&s->clients[i];if(c->socket==INVALID)continue;short events=fds[i+1].revents;
   if(now>=c->deadline||(events&(POLLERR|POLLHUP|POLLNVAL))||
      ((events&POLLIN)&&!c->ready&&receive_header(s,c,now))||((events&POLLOUT)&&c->ready&&send_part(s,c,now)))drop(c);
  }
 }
 for(unsigned i=0;i<CLIENTS;i++)drop(&s->clients[i]);
 atomic_store(&s->alive,0);
}
#ifdef _WIN32
static unsigned __stdcall run(void* arg){serve(arg);return 0;}
#else
static void* run(void* arg){serve(arg);return NULL;}
#endif
int pm_package_source_start(PmPackageSource** out,int fd,const char* request,uint64_t bytes,char* url,size_t capacity){
 if(!out)return -1;*out=NULL;if(!url||capacity<100||fd<0||!request||!pm_package_hex(request,32)||!bytes||bytes>PM_PACKAGE_MAX)return -1;
#ifdef _WIN32
 WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))return -1;
#endif
 PmPackageSource* s=calloc(1,sizeof *s);if(!s)goto network_fail;s->listener=INVALID;s->file=-1;s->bytes=bytes;
 for(unsigned i=0;i<CLIENTS;i++)s->clients[i].socket=INVALID;
 atomic_init(&s->stop,0);atomic_init(&s->alive,1);atomic_init(&s->fault,0);
 s->file=file_dup(fd);if(s->file<0||file_stat(s->file,&s->original)||s->original.st_size<0||(uint64_t)s->original.st_size!=bytes)goto fail;
 snprintf(s->target,sizeof s->target,"/%s/package.pkg",request);
 s->listener=socket(AF_INET,SOCK_STREAM,0);if(s->listener==INVALID||nonblock(s->listener))goto fail;
 struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(s->listener,(struct sockaddr*)&address,sizeof address)||listen(s->listener,CLIENTS))goto fail;
#ifdef _WIN32
 int size=sizeof address;
#else
 socklen_t size=sizeof address;
#endif
 if(getsockname(s->listener,(struct sockaddr*)&address,&size)||!address.sin_port)goto fail;
 int n=snprintf(url,capacity,"http://127.0.0.1:%u%s",(unsigned)ntohs(address.sin_port),s->target);if(n<0||(size_t)n>=capacity)goto fail;
#ifdef _WIN32
 s->thread=(HANDLE)_beginthreadex(NULL,0,run,s,0,NULL);if(!s->thread)goto fail;
#else
 if(pthread_create(&s->thread,NULL,run,s))goto fail;
#endif
 *out=s;return 0;
fail:
 if(s->listener!=INVALID)close_socket(s->listener);if(s->file>=0)file_close(s->file);free(s);
network_fail:
#ifdef _WIN32
 WSACleanup();
#endif
 return -1;
}
int pm_package_source_healthy(PmPackageSource* s){return s&&atomic_load(&s->alive)&&!atomic_load(&s->fault);}
void pm_package_source_stop(PmPackageSource* s){
 if(!s)return;atomic_store(&s->stop,1);
#ifdef _WIN32
 WaitForSingleObject(s->thread,INFINITE);CloseHandle(s->thread);
#else
 pthread_join(s->thread,NULL);
#endif
 close_socket(s->listener);file_close(s->file);free(s);
#ifdef _WIN32
 WSACleanup();
#endif
}
