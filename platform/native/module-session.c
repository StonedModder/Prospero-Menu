// SPDX-License-Identifier: GPL-3.0-or-later
#include "module-session.h"
#include "game-info.h"
#include "sha256.h"
#include "goldhen-compat.h"
#include <string.h>
#include <stdio.h>
int pm_hex_string(const char *s,size_t n){if(!s||strlen(s)!=n)return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static void digest(const PmModuleSession *s,unsigned char out[32]){SHA256_CTX h;sha256_init(&h);sha256_update(&h,(const unsigned char*)s,offsetof(PmModuleSession,checksum));sha256_final(&h,out);}
void pm_module_seal(PmModuleSession *s){digest(s,s->checksum);}
int pm_module_decode(const void *bytes,size_t length,const char *generation,PmModuleSession *s){
  if(!bytes||!generation||length!=sizeof *s)return -1;memcpy(s,bytes,sizeof *s);unsigned char hash[32];digest(s,hash);
  if(memcmp(hash,s->checksum,32)||s->magic!=PM_MODULE_SESSION_MAGIC||s->version!=PM_MODULE_SESSION_VERSION||s->phase>PM_ARENA_UNKNOWN||s->pid<=1||
    s->generation[127]||s->title[9]||s->metadataHash[64]||strcmp(s->generation,generation)||!pm_game_title(s->title)||!pm_hex_string(s->metadataHash,64))return -1;
  if(s->arenaRequest[32]||s->arenaMessage[191]||s->arenaStage>4||
    (s->arenaStage&&!pm_hex_string(s->arenaRequest,32))||(!s->arenaStage&&s->arenaRequest[0])||
    (s->arenaStage==2&&s->phase!=PM_ARENA_READY&&s->phase!=PM_ARENA_UNKNOWN))return -1;
  if(s->phase==PM_ARENA_READY&&(!s->cookie||s->arena<0x10000||s->arena>UINT64_C(0x7fffffffffff)-0x8000||
    (s->arena&0x3fff)||s->control!=s->arena+0x4000))return -1;
  for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++){
    const PmModuleReceipt *r=&s->slots[i];
    if(r->stage>PM_PRX_REJECTED||r->clean>1||r->request[32]||r->upload[32]||r->sha256[64]||r->extension[7]||r->message[191])return -1;
    if(!r->stage){PmModuleReceipt empty={0};if(memcmp(r,&empty,sizeof empty))return -1;continue;}
    if(!pm_hex_string(r->request,32)||!pm_hex_string(r->upload,32)||!pm_hex_string(r->sha256,64)||
      (strcmp(r->extension,"prx")&&strcmp(r->extension,"sprx")))return -1;
    if(r->stage==PM_PRX_STARTED&&(r->handle<0||r->startResult<0||r->clean))return -1;
    if(r->clean&&r->stage!=PM_PRX_QUEUED&&r->stage!=PM_PRX_REJECTED)return -1;
    for(unsigned j=0;j<i;j++)if(s->slots[j].stage&&!strcmp(s->slots[j].request,r->request))return -1;
  }
  return 0;
}
int pm_module_slot(const PmModuleSession *s,const char *request){for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++)if(s->slots[i].stage&&!strcmp(s->slots[i].request,request))return (int)i;return -1;}
int pm_module_session_held(const PmModuleSession *s){
  if(!s||s->phase==PM_ARENA_INTENT||s->phase==PM_ARENA_UNKNOWN||s->arenaStage==1||s->arenaStage==4)return 1;
  for(unsigned i=0;i<PM_GAME_MODULE_SLOTS;i++)if(s->slots[i].stage==PM_PRX_QUEUED||s->slots[i].stage==PM_PRX_ARMING||s->slots[i].stage==PM_PRX_UNKNOWN)return 1;
  return 0;
}
int pm_module_arena_claim(PmModuleSession *s,const char *request){
  if(!s||!pm_hex_string(request,32))return -1;
  if(s->arenaRequest[0])return strcmp(s->arenaRequest,request)?-1:1;
  if(pm_module_session_held(s))return -1;
  strcpy(s->arenaRequest,request);s->arenaStage=1;snprintf(s->arenaMessage,sizeof s->arenaMessage,"Game input preparation queued");return 0;
}
void pm_module_path(const PmModuleReceipt *r,char path[256]){
  // The private Havana copy contains per-process gate addresses. Never reuse
  // it for another request/game generation or overwrite a mapped old inode.
  if(pm_goldhen_supported(r->sha256))snprintf(path,256,PM_GAME_MODULE_PREFIX "%s%s.%s",r->request,r->request,r->extension);
  else snprintf(path,256,PM_GAME_MODULE_PREFIX "%s.%s",r->sha256,r->extension);
}
void pm_module_failure_message(int32_t handle,int32_t startResult,char *message,size_t size){
  if((uint32_t)handle==UINT32_C(0x80020063)){
    snprintf(message,size,"Game loader rejected the module's SDK version (0x80020063). Check plugin/game compatibility. Close the game before retrying; other payloads remain available.");return;
  }
  if(handle<0)snprintf(message,size,"Game module loader rejected file: 0x%08x%s; start result 0x%08x. Close the game before retrying.",
    (unsigned)handle,(uint32_t)handle==UINT32_C(0x8002000d)?" (access denied)":"",(unsigned)startResult);
  else snprintf(message,size,"Game module initializer failed: 0x%08x (handle %d); module may remain resident until this game closes.",(unsigned)startResult,handle);
}
int pm_module_gate_valid(const PmModuleSession *s,const PmGameGate *g){
  return s->phase==PM_ARENA_READY&&g->magic==PM_GAME_GATE_MAGIC&&g->version==PM_GAME_GATE_VERSION&&g->bytes==sizeof *g&&g->cookie==s->cookie&&
    g->enabled==1&&g->busy<=1&&g->loadStartModule>=0x10000&&g->loadStartModule<UINT64_C(0x7fffffffffff)&&
    g->threadCreate>=0x10000&&g->threadCreate<UINT64_C(0x7fffffffffff)&&
    g->threadDetach>=0x10000&&g->threadDetach<UINT64_C(0x7fffffffffff)&&
    g->dlsym>=0x10000&&g->dlsym<UINT64_C(0x7fffffffffff);
}
int pm_module_stage(const PmModuleSession *s,unsigned slot,const PmModuleOps *ops){
  if(!s||!ops||!ops->read||!ops->write||slot>=PM_GAME_MODULE_SLOTS||s->slots[slot].stage!=PM_PRX_ARMING)return 1;
  PmGameGate g;PmGameModule empty={0},pending={0};
  if(ops->read(ops->context,&g)||!pm_module_gate_valid(s,&g)||memcmp(&g.modules[slot],&empty,sizeof empty))return 1;
  // The target is stopped, so a callback observed busy cannot finish until
  // the caller resumes it. Nothing has been written: this is safe to wait on.
  if(g.busy)return 2;
  pm_module_path(&s->slots[slot],pending.path);size_t at=offsetof(PmGameGate,modules)+slot*sizeof pending;
  if(ops->write(ops->context,at,&pending,sizeof pending)||ops->read(ops->context,&g)||!pm_module_gate_valid(s,&g)||
    memcmp(&g.modules[slot],&pending,sizeof pending))goto rollback;
  pending.enabled=1;
  if(ops->write(ops->context,at,&pending.enabled,sizeof pending.enabled)||ops->read(ops->context,&g)||!pm_module_gate_valid(s,&g)||
    memcmp(&g.modules[slot],&pending,sizeof pending))goto rollback;
  return 0;
rollback:
  // No thread may execute until the caller has finished this rollback.
  if(ops->write(ops->context,at,&empty,sizeof empty)||ops->read(ops->context,&g)||!pm_module_gate_valid(s,&g)||
    memcmp(&g.modules[slot],&empty,sizeof empty))return -1;
  return 1;
}
