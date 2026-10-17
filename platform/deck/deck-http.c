// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck-http.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
static uint64_t millis(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int wait_fd(int fd,short events,uint64_t deadline){
 for(;;){uint64_t now=millis();if(!now||now>=deadline)return 0;struct pollfd p={fd,events,0};
  int n=poll(&p,1,(int)(deadline-now));if(n<0&&errno==EINTR)continue;
  return n>0&&(p.revents&(events|POLLHUP))&&!(p.revents&(POLLERR|POLLNVAL));}
}
static int request(int cheats,uint16_t port,const char credential[33],const PmDeckEvent* event,const PmDeckFeedback* feedback,PmDeckFrame* frame){
 if(!port||!credential||!frame||!feedback||credential[32])return -1;
 for(unsigned i=0;i<32;i++)if(!((credential[i]>='a'&&credential[i]<='f')||(credential[i]>='0'&&credential[i]<='9')))return -1;
 char action[192]="null",dashboard[128]="null",body[896],request[1328];
 if(feedback->dashboardSerial){
  if(feedback->dashboardTitle[9])return -1;
  for(unsigned i=0;i<9;i++)if(!((feedback->dashboardTitle[i]>='A'&&feedback->dashboardTitle[i]<='Z')||(feedback->dashboardTitle[i]>='0'&&feedback->dashboardTitle[i]<='9')))return -1;
  snprintf(dashboard,sizeof dashboard,"{\"serial\":%u,\"titleId\":\"%.9s\"}",feedback->dashboardSerial,feedback->dashboardTitle);
 }
  if(event){const char* kind=event->kind==1?"select":event->kind==2?"next":event->kind==3?"previous":event->kind==4?"back":event->kind==5?"window":NULL;
  if(!kind||!event->serial)return -1;
  // Session-specific request prefix + monotonically increasing serial.
  int n=snprintf(action,sizeof action,"{\"kind\":\"%s\",\"sequence\":%u,\"row\":%u,\"request\":\"%.24s%08x\"}",kind,event->sequence,event->row,credential,event->serial);
  if(n<0||(size_t)n>=sizeof action)return -1;
 }
 int n=snprintf(body,sizeof body,"{\"feedback\":{\"epoch\":%u,\"serial\":%u,\"open\":%s,\"owner\":%d,\"connection\":%u,\"ready\":%s,\"renderedAt\":%llu,\"routeResets\":[%u,%u,%u,%u]},\"menuOpen\":%s,\"dashboard\":%s,\"action\":%s}",
  feedback->epoch,feedback->serial,feedback->open?"true":"false",feedback->owner,feedback->connection,feedback->ready?"true":"false",(unsigned long long)feedback->renderedAt,
  feedback->routeResets[0],feedback->routeResets[1],feedback->routeResets[2],feedback->routeResets[3],feedback->menuOpen?"true":"false",dashboard,action);
 if(n<0||(size_t)n>=sizeof body)return -1;size_t bodylen=(size_t)n;
 n=snprintf(request,sizeof request,"POST /native/%s/sync HTTP/1.0\r\nHost: 127.0.0.1:%u\r\nX-ProsperoMenu-Native: %s\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",cheats?"cheats":"deck",port,credential,bodylen,body);
 if(n<0||(size_t)n>=sizeof request)return -1;size_t length=(size_t)n;
 int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;int outcome=-1;
 if(fcntl(fd,F_SETFL,O_NONBLOCK)<0)goto done;
 struct sockaddr_in address={0};address.sin_len=sizeof address;address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(0x7f000001);
 uint64_t deadline=millis()+2000;
 if(connect(fd,(struct sockaddr*)&address,sizeof address)&&errno!=EINPROGRESS)goto done;
 if(!wait_fd(fd,POLLOUT,deadline))goto done;int error=0;socklen_t errorlen=sizeof error;
 if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&errorlen)||error)goto done;
 for(size_t off=0;off<length;){if(!wait_fd(fd,POLLOUT,deadline))goto done;ssize_t count=send(fd,request+off,length-off,MSG_NOSIGNAL);
  if(count<0&&(errno==EAGAIN||errno==EINTR))continue;if(count<=0)goto done;off+=(size_t)count;}
 unsigned char response[sizeof(PmDeckFrame)+2048];size_t used=0;
 for(;;){if(used==sizeof response)goto done;if(!wait_fd(fd,POLLIN,deadline))goto done;
  ssize_t count=recv(fd,response+used,sizeof response-used,0);
  if(count<0&&(errno==EAGAIN||errno==EINTR))continue;if(count<0)goto done;if(!count)break;used+=(size_t)count;}
 if(used<16||memcmp(response,"HTTP/1.",7))goto done;
 if((response[7]!='0'&&response[7]!='1')||response[8]!=' '||response[9]<'1'||response[9]>'5'||response[10]<'0'||response[10]>'9'||response[11]<'0'||response[11]>'9')goto done;
 int status=(response[9]-'0')*100+(response[10]-'0')*10+(response[11]-'0');
 size_t split=0;for(size_t i=12;i+3<used;i++)if(!memcmp(response+i,"\r\n\r\n",4)){split=i+4;break;}
 if(!split||split>4096)goto done;
 if(pm_deck_http_has_frame(status)){if(!pm_deck_decode(response+split,used-split,frame))goto done;}
 outcome=status;
done:close(fd);return outcome;
}

int pm_deck_http(uint16_t p,const char*c,const PmDeckEvent*e,const PmDeckFeedback*f,PmDeckFrame*out){return request(0,p,c,e,f,out);}
int pm_cheats_http(uint16_t p,const char*c,const PmDeckEvent*e,const PmDeckFeedback*f,PmDeckFrame*out){return request(1,p,c,e,f,out);}
