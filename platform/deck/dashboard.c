// SPDX-License-Identifier: GPL-3.0-or-later
// Original ProsperoMenu dashboard decoration and Options action. No database writes,
// dashboard reloads, file reads, or network calls run inside managed callbacks.
#include "dashboard.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <ps5/kernel.h>
static PmMono* m;static void* domain;
static unsigned prepareError;
static int(*string_length)(void*);static const uint16_t*(*string_chars)(void*);
static void *createJson,*reloadHome;
static unsigned homeSetting,homeEpoch,versionInstalled;
static void(*textOriginal)(void*,void*);
static int(*registryOriginal)(long,int*);
static volatile PmDeckBoot* trace;
void pm_dashboard_bind(volatile PmDeckBoot* b){trace=b;}
#define COUNT(field) do{if(trace)__atomic_fetch_add(&trace->field,1,__ATOMIC_RELAXED);}while(0)
static void(*jsonOriginal)(void*,void*,void*,void*,void*,void*,void*,void*,uint8_t);
static int(*urlOriginal)(void*,void*,void*);
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static PmDashboardRecord cache[16384];static uint32_t epoch,enabled,serial,acknowledged;static char requested[10];
static int title_valid(const char*s){
 if(!s||strlen(s)!=9)return 0;const char*prefix[]={"PPSA","PPSB","MOUU","CUSA","PCAS","PCJS","PCKS","CUHJ","SCUS"};int found=0;
 for(unsigned i=0;i<sizeof prefix/sizeof prefix[0];i++)if(!memcmp(s,prefix[i],4))found=1;
 for(unsigned i=4;i<9;i++)if(s[i]<'0'||s[i]>'9')return 0;return found;
}
static int ascii(void*str,char*out,size_t cap){
 if(!str)return 0;int n=string_length(str);if(n<0||(size_t)n>=cap)return 0;const uint16_t*s=string_chars(str);if(!s)return 0;
 for(int i=0;i<n;i++){if(s[i]<32||s[i]>126)return 0;out[i]=(char)s[i];}out[n]=0;return 1;
}
static void* invoke(void*fn,void*object,void**args){if(!fn)return NULL;void*ex=NULL;void*r=m->mono_runtime_invoke(fn,object,args,&ex);return ex?NULL:r;}
static void* meth(void*c,const char*n,int count,int ret,const int*params,int isStatic){
 void*it=NULL,*found=NULL,*fn;while(c&&(fn=m->mono_class_get_methods(c,&it))){
  if(strcmp(m->mono_method_get_name(fn),n)||!!(m->mono_method_get_flags(fn,NULL)&16)!=isStatic)continue;
  void*s=m->mono_method_signature(fn);if(!s||m->mono_signature_get_param_count(s)!=(unsigned)count||m->mono_type_get_type(m->mono_signature_get_return_type(s))!=ret)continue;
  void*p=NULL;int i;for(i=0;i<count;i++)if(m->mono_type_get_type(m->mono_signature_get_params(s,&p))!=params[i])break;
  if(i!=count)continue;if(found)return NULL;found=fn;
 }return found;
}
static void* klass(const char*assembly,const char*ns,const char*name){char p[192];snprintf(p,sizeof p,"/system_ex/common_ex/lib/%s.dll",assembly);void*a=m->mono_domain_assembly_open(domain,p);return a?m->mono_class_from_name(m->mono_assembly_get_image(a),ns,name):NULL;}
static unsigned slot(const char*s){uint32_t n=2166136261u;for(unsigned i=0;i<9;i++)n=(n^(unsigned char)s[i])*16777619u;return n&16383;}
void pm_dashboard_receive(const PmDashboardFrame*f){
 pthread_mutex_lock(&lock);__atomic_store_n(&enabled,f->enabled,__ATOMIC_RELAXED);
 if(epoch!=f->epoch){memset(cache,0,sizeof cache);__atomic_store_n(&epoch,f->epoch,__ATOMIC_RELAXED);}
 for(unsigned i=0;i<f->count;i++)if(title_valid(f->records[i].title)){
  unsigned start=slot(f->records[i].title);for(unsigned j=0;j<16384;j++){PmDashboardRecord*r=&cache[(start+j)&16383];if(!r->title[0]||!strcmp(r->title,f->records[i].title)){*r=f->records[i];break;}}
 }
 pthread_mutex_unlock(&lock);
}
// RegistryUtil.ReadBoolean builds an object containing the flag, not a bare
// JValue. Override the underlying integer read so the native response envelope
// and success result are constructed by ShellUI itself. Resolved by export,
// with no firmware address table or persistent registry writes.
static int read_registry(long key,int*value){
 if(key==0x7802c916L){
  COUNT(dashboardTitleCalls);
  if(value&&__atomic_load_n(&enabled,__ATOMIC_RELAXED)){
   *value=1;COUNT(dashboardTitleDecorated);return 0;
  }
 }
 return registryOriginal(key,value);
}
// ReactRawTextShadowNode.SetText consumes display text only. In contrast to
// database/get_TitleId hooks, this never changes a launch or catalog identifier.
static void version_text(void*object,void*text){
 char title[10],version[16]={0};
 if(!object||!__atomic_load_n(&enabled,__ATOMIC_RELAXED)||!ascii(text,title,sizeof title)||!title_valid(title)){textOriginal(object,text);return;}
 COUNT(dashboardVersionCalls);
 if(!pthread_mutex_trylock(&lock)){
  unsigned start=slot(title);for(unsigned i=0;i<16384;i++){
   PmDashboardRecord*r=&cache[(start+i)&16383];if(!r->title[0])break;
   if(!strcmp(r->title,title)){memcpy(version,r->version,sizeof version);break;}
  }
  pthread_mutex_unlock(&lock);
 }
 if(!version[0]||version[15]){textOriginal(object,text);return;}
 for(unsigned i=0;version[i];i++)if((version[i]<'0'||version[i]>'9')&&version[i]!='.'){textOriginal(object,text);return;}
 uint32_t root=m->mono_gchandle_new(object,1);if(!root){textOriginal(object,text);return;}
 char label[48];snprintf(label,sizeof label,"%s | v%s",title,version);void*value=m->mono_string_new(domain,label);
 if(value){textOriginal(m->mono_gchandle_get_target(root),value);COUNT(dashboardVersionDecorated);}else textOriginal(m->mono_gchandle_get_target(root),text);
 m->mono_gchandle_free(root);
}
int pm_dashboard_version_prepare(void**target,void**replacement){
 void*thread=m->mono_thread_attach(domain);if(!thread)return 0;
 void*c=klass("ReactNative.PUI","ReactNative.Views.UI3.View","ReactRawTextShadowNode");const int str[]={0xe};
 void*f=meth(c,"SetText",1,1,str,0);*target=f?m->mono_compile_method(f):NULL;*replacement=version_text;
 m->mono_thread_detach(thread);return *target!=NULL;
}
void pm_dashboard_version_original(void*original){textOriginal=original;versionInstalled=1;}
static int title_from_url(void*url,char title[10]){
 char text[512];const char prefix[]="pspatchcheck:check-for-update?titleid=";
 if(!ascii(url,text,sizeof text)||strncmp(text,prefix,sizeof prefix-1))return 0;
 const char*p=text+sizeof prefix-1;size_t n=strcspn(p,"&#");if(n!=9)return 0;
 memcpy(title,p,9);title[9]=0;return title_valid(title);
}
// AOT AddCheckPatch passes rsi=array, rdx=id, r8=actionUrl. It does NOT
// preserve rdi=this: createJson never uses it. Never dereference/pin object.
static void add_json(void*object,void*array,void*id,void*label,void*url,void*action,void*message,void*submenu,uint8_t enable){
 COUNT(dashboardOptionCalls);char row[64],title[10];
 int add=array&&ascii(id,row,sizeof row)&&!strcmp(row,"MENU_ID_CHECK_PATCH")&&title_from_url(url,title);
 uint32_t roots[4]={add?m->mono_gchandle_new(array,1):0,0,0,0};
 jsonOriginal(object,array,id,label,url,action,message,submenu,enable);
 if(!add||!roots[0])return;
 char uri[64];snprintf(uri,sizeof uri,"prosperomenu://cheats/%s",title);
 const char*text[]={"prosperomenu.cheats","ProsperoMenu Cheats",uri};int ok=1;
 for(unsigned i=0;i<3;i++){void*s=m->mono_string_new(domain,text[i]);roots[i+1]=s?m->mono_gchandle_new(s,1):0;if(!roots[i+1])ok=0;}
 if(ok){jsonOriginal(NULL,m->mono_gchandle_get_target(roots[0]),
  m->mono_gchandle_get_target(roots[1]),m->mono_gchandle_get_target(roots[2]),m->mono_gchandle_get_target(roots[3]),NULL,NULL,NULL,1);COUNT(dashboardOptionAdded);}
 for(unsigned i=0;i<4;i++)if(roots[i])m->mono_gchandle_free(roots[i]);
}
// Both OpenURL and OpenURLArg call this validator before launching anything.
// Fulfil our local action and tell the caller it has already been handled.
// Every other URL follows the original validation and launch path unchanged.
static int intercept_url(void*object,void*url,void*promise){
 COUNT(dashboardUrlCalls);char uri[96];if(!ascii(url,uri,sizeof uri)||strncmp(uri,"prosperomenu://cheats/",sizeof "prosperomenu://cheats/"-1)||!title_valid(uri+(sizeof "prosperomenu://cheats/"-1))||!promise)return urlOriginal(object,url,promise);
 const int types[]={0x1c};void*resolve=NULL,*c=m->mono_object_get_class(promise);
 for(unsigned i=0;c&&!resolve&&i<12;i++,c=m->mono_class_get_parent(c))resolve=meth(c,"Resolve",1,1,types,0);
 if(!resolve)return urlOriginal(object,url,promise);
 pthread_mutex_lock(&lock);memcpy(requested,uri+16,10);if(!++serial)serial=1;pthread_mutex_unlock(&lock);
 void*args[]={NULL};invoke(resolve,promise,args);return 1;
}
void pm_dashboard_feedback(PmDeckFeedback*f){pthread_mutex_lock(&lock);if(serial!=acknowledged){f->dashboardSerial=serial;memcpy(f->dashboardTitle,requested,10);}pthread_mutex_unlock(&lock);}
int pm_dashboard_ack(uint32_t ack){int open=0;pthread_mutex_lock(&lock);if(ack&&ack==serial&&ack!=acknowledged){acknowledged=ack;open=1;}pthread_mutex_unlock(&lock);return open;}
// UI thread only, after our overlay closes and only while ShellUI has focus.
// Coalesce toggle changes into one home-scene refresh; never loop/retry it.
void pm_dashboard_tick(int idle){
 unsigned next=__atomic_load_n(&enabled,__ATOMIC_RELAXED),revision=__atomic_load_n(&epoch,__ATOMIC_RELAXED);
 if(!idle||!reloadHome||(next==homeSetting&&(!next||!versionInstalled||homeEpoch==revision)))return;
 homeSetting=next;homeEpoch=revision;void*title=m->mono_string_new(domain,"NPXS40002"),*args[]={title},*ex=NULL;
 if(title)m->mono_runtime_invoke(reloadHome,NULL,args,&ex);
 if(trace){trace->dashboardRefreshes++;trace->dashboardRefreshError=ex?1:0;}
}
void pm_dashboard_originals(void*t[3]){registryOriginal=t[0];jsonOriginal=t[1];urlOriginal=t[2];}
int pm_dashboard_prepare(PmMono*api,void*d,void*targets[3],void*replacements[3]){
 prepareError=1;m=api;domain=d;string_length=m->mono_string_length;string_chars=m->mono_string_chars;if(!string_length||!string_chars)return 0;
 prepareError=2;void*thread=m->mono_thread_attach(domain);if(!thread)return 0;int ok=0;
 void*options=klass("Sce.Vsh.ShellUI.ReactNativeShellApp","ReactNative.Modules.ShellUI.HomeUI","OptionMenu");
 void*link=klass("ReactNative.Modules.Vsh","ReactNative.Modules.Vsh.Application","LinkingPSModule");
 void*classes[]={options,link};for(unsigned i=0;i<2;i++)if(!classes[i]){prepareError=10+i;goto done;}
 const int jsonTypes[]={0x12,0xe,0xe,0xe,0xe,0xe,0x12,2},urlTypes[]={0xe,0x12};
 createJson=meth(options,"createJson",8,1,jsonTypes,0);
 if(!createJson){prepareError=30;goto done;}
 uint32_t reg=0;
 if(kernel_dynlib_handle(getpid(),"libSceRegMgr.sprx",&reg)||!(targets[0]=(void*)kernel_dynlib_dlsym(getpid(),reg,"sceRegMgrGetInt"))){prepareError=31;goto done;}
 void*methods[]={createJson,meth(link,"RejectInvalidURL",2,2,urlTypes,0)};
 for(unsigned i=0;i<2;i++){if(!methods[i]){prepareError=33+i;goto done;}targets[i+1]=m->mono_compile_method(methods[i]);if(!targets[i+1]){prepareError=41+i;goto done;}}
 replacements[0]=read_registry;replacements[1]=add_json;replacements[2]=intercept_url;void*sceneManager=klass("ReactNative.Vsh.Common","ReactNative.Vsh.Common","ReactApplicationSceneManager");
 const int titleType[]={0xe};reloadHome=meth(sceneManager,"ReloadApp",1,1,titleType,1);
 prepareError=0;ok=1;
done:m->mono_thread_detach(thread);return ok;
}
unsigned pm_dashboard_prepare_error(void){return prepareError;}
