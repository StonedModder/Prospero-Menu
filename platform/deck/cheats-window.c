// SPDX-License-Identifier: GPL-3.0-or-later
// Native settings-page integration researched from the console's managed
// metadata/AOT. etaHEN's XML settings approach and OnionHEN's legacy URI route
// informed this integration; no debug-settings resources or files are replaced.
#include "cheats-window.h"
#include "dashboard.h"
#include "deck-http.h"
#include <ps5/kernel.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
static PmMono*m;static void*domain;static volatile PmDeckBoot*trace;
static void *managerField,*push,*getId,*encodingUtf8,*getBytes,*streamClass,*streamCtor;
static void *addItem,*removeItem,*focusItem,*getFocused,*getActive,*getElement,*buttonClass,*labelClass,*buttonCtor,*labelCtor,*setId,*setTitle,*setRawTitle,*setDetail;
static void(*startOriginal)(void*);static void*(*fileOriginal)(void*,void*);
static void(*pressOriginal)(void*,void*,void*);static void(*unloadOriginal)(void*);
static int(*initialize)(void),(*foreground)(int*),(*launch)(const char*,void*);
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static PmDeckFrame incoming,shown;static PmDeckEvent action;
static unsigned installed,opening,active,requestSerial,actionSerial,actionPending,refresh;
static unsigned managerSeen;static uint64_t checkAt,managerDeadline;
static uint32_t appRoot,managerRoot;static uint64_t launchDeadline;static char xml[16384];
static const char resource[]="prosperomenu_cheats.xml",rootId[]="prosperomenu_cheats";
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void*call(void*f,void*o,void**a){void*ex=NULL;void*r=m->mono_runtime_invoke(f,o,a,&ex);return ex?NULL:r;}
static int ascii(void*s,char*out,size_t n){if(!s)return 0;int len=m->mono_string_length(s);if(len<0||(size_t)len>=n)return 0;const uint16_t*p=m->mono_string_chars(s);if(!p)return 0;for(int i=0;i<len;i++){if(p[i]<32||p[i]>126)return 0;out[i]=(char)p[i];}out[len]=0;return 1;}
static void*klass(const char*a,const char*ns,const char*n){char path[192];snprintf(path,sizeof path,"/system_ex/common_ex/lib/%s.dll",a);void*assembly=m->mono_domain_assembly_open(domain,path);return assembly?m->mono_class_from_name(m->mono_assembly_get_image(assembly),ns,n):NULL;}
static void*method(void*c,const char*n,int count,int ret,const int*types,int stat){void*it=NULL,*f,*found=NULL;while(c&&(f=m->mono_class_get_methods(c,&it))){if(strcmp(m->mono_method_get_name(f),n)||!!(m->mono_method_get_flags(f,NULL)&16)!=stat)continue;void*s=m->mono_method_signature(f);if(!s||m->mono_signature_get_param_count(s)!=(unsigned)count||m->mono_type_get_type(m->mono_signature_get_return_type(s))!=ret)continue;void*pi=NULL;int i=0;for(;i<count;i++)if(m->mono_type_get_type(m->mono_signature_get_params(s,&pi))!=types[i])break;if(i!=count)continue;if(found)return NULL;found=f;}return found;}
static void clear_roots(void){if(appRoot)m->mono_gchandle_free(appRoot);if(managerRoot)m->mono_gchandle_free(managerRoot);appRoot=managerRoot=0;}
static void start(void*object){
 if(!__atomic_load_n(&opening,__ATOMIC_ACQUIRE)||now()>launchDeadline){startOriginal(object);return;}
 __atomic_store_n(&opening,0,__ATOMIC_RELEASE);if(trace)trace->cheatsStage=3;
 void*manager=m->mono_field_get_value_object(domain,managerField,object);if(!manager){if(trace)trace->cheatsError=3;startOriginal(object);return;}
 clear_roots();appRoot=m->mono_gchandle_new(object,1);managerRoot=m->mono_gchandle_new(manager,1);
 if(!appRoot||!managerRoot){clear_roots();if(trace)trace->cheatsError=4;startOriginal(object);return;}
 managerSeen=0;checkAt=0;managerDeadline=now()+10000;
 __atomic_store_n(&active,1,__ATOMIC_RELEASE);
 void*file=m->mono_string_new(domain,resource),*root=m->mono_string_new(domain,rootId);int animation=0;
 void*args[]={file,root,&animation},*ex=NULL;m->mono_runtime_invoke(push,m->mono_gchandle_get_target(managerRoot),args,&ex);
 if(ex){if(trace)trace->cheatsError=5;__atomic_store_n(&active,0,__ATOMIC_RELEASE);clear_roots();startOriginal(object);}else if(trace)trace->cheatsStage=4;
}
static void*read_file(void*object,void*file){
 char name[64];if(!ascii(file,name,sizeof name)||strcmp(name,resource))return fileOriginal(object,file);
 // Only this private virtual resource is supplied. Ordinary settings keep their
 // native stream. No disk reads, parsing, downloads or jobs run in this hook.
 char copy[sizeof xml];pthread_mutex_lock(&guard);memcpy(copy,xml,sizeof copy);pthread_mutex_unlock(&guard);
 uint32_t roots[3]={0};void*result=NULL,*utf8=call(encodingUtf8,NULL,NULL);
 void*text=m->mono_string_new(domain,copy);if(!utf8||!text)return NULL;roots[0]=m->mono_gchandle_new(text,1);if(!roots[0])return NULL;
 void*args[]={m->mono_gchandle_get_target(roots[0])};void*bytes=call(getBytes,utf8,args);if(!bytes)goto done;roots[1]=m->mono_gchandle_new(bytes,1);if(!roots[1])goto done;
 void*stream=m->mono_object_new(domain,streamClass);if(!stream)goto done;roots[2]=m->mono_gchandle_new(stream,1);if(!roots[2])goto done;
 uint8_t writable=0;void*ctorArgs[]={m->mono_gchandle_get_target(roots[1]),&writable},*ex=NULL;
 m->mono_runtime_invoke(streamCtor,m->mono_gchandle_get_target(roots[2]),ctorArgs,&ex);if(!ex)result=m->mono_gchandle_get_target(roots[2]);
 done:for(unsigned i=0;i<3;i++)if(roots[i])m->mono_gchandle_free(roots[i]);return result;
}
static void press(void*object,void*element,void*event){
 char id[96];void*s=element?call(getId,element,NULL):NULL;
 if(!ascii(s,id,sizeof id)||strncmp(id,"prosperomenu.cheat.",sizeof "prosperomenu.cheat."-1)){pressOriginal(object,element,event);return;}
 if(strlen(id+(sizeof "prosperomenu.cheat."-1))!=32)return;
 pthread_mutex_lock(&guard);
 unsigned row=0;for(;row<shown.count;row++)if(!memcmp(id+(sizeof "prosperomenu.cheat."-1),shown.rows[row].token,32))break;
 if(active&&!actionPending&&row<shown.count&&(shown.rows[row].flags&1)){
  action=(PmDeckEvent){.kind=PM_DECK_SELECT,.sequence=shown.sequence,.row=row,.serial=++actionSerial};if(!action.serial)action.serial=++actionSerial;actionPending=1;
  if(trace){trace->cheatsActions++;trace->cheatsStage=5;}
 }
 pthread_mutex_unlock(&guard);
}
static void unloaded(void*object){
 if(appRoot&&m->mono_gchandle_get_target(appRoot)==object){__atomic_store_n(&active,0,__ATOMIC_RELEASE);__atomic_store_n(&opening,0,__ATOMIC_RELEASE);clear_roots();if(trace)trace->cheatsStage=6;}
 unloadOriginal(object);
}
// Bounded UTF-8 XML escaping. All content is data, never markup or callbacks.
static int append(char*out,size_t cap,size_t*at,const char*s){size_t n=strlen(s);if(n>=cap-*at)return 0;memcpy(out+*at,s,n+1);*at+=n;return 1;}
static int escape(char*out,size_t cap,size_t*at,const char*s){for(;*s;s++){const char*p=*s=='&'?"&amp;":*s=='<'?"&lt;":*s=='>'?"&gt;":*s=='\"'?"&quot;":*s=='\''?"&apos;":NULL;char one[]={*s,0};if(!append(out,cap,at,p?p:one))return 0;}return 1;}
static int make_xml(const PmDeckFrame*f,char*out,size_t cap){size_t at=0;char line[96];
#define A(s) do{if(!append(out,cap,&at,s))return 0;}while(0)
#define E(s) do{if(!escape(out,cap,&at,s))return 0;}while(0)
 A("<?xml version=\"1.0\" encoding=\"UTF-8\"?><system_settings version=\"1.0\" plugin=\"debug_settings_plugin\"><setting_list id=\"prosperomenu_cheats\" title=\"ProsperoMenu Cheats\">");
 A("<label id=\"prosperomenu_info\" title=\"");E(f->subtitle);A("\"/>");
 for(unsigned i=0;i<f->count&&i<PM_DECK_ROWS;i++){const PmDeckRow*r=&f->rows[i];snprintf(line,sizeof line,"<%s id=\"prosperomenu.cheat.%.*s\" title=\"",r->flags&1?"button":"label",32,r->token);A(line);if(r->flags&2)A("[ON] ");E(r->label);A("\" second_title=\"");E(r->detail);A("\"/>");}
 A("</setting_list></system_settings>");return 1;
#undef A
#undef E
}
static int set_string(void*method,void*object,const char*value){void*s=m->mono_string_new(domain,value),*a[]={s},*ex=NULL;if(!s)return 0;m->mono_runtime_invoke(method,object,a,&ex);return !ex;}
static int add_row(void*manager,const char*id,const char*title,const char*detail,int enabled){
 void*o=m->mono_object_new(domain,enabled?buttonClass:labelClass);if(!o)return 0;uint32_t h=m->mono_gchandle_new(o,1);if(!h)return 0;
 void*ex=NULL;m->mono_runtime_invoke(enabled?buttonCtor:labelCtor,o,NULL,&ex);int ok=!ex;
 if(ok)ok=set_string(setId,o,id)&&set_string(setTitle,o,title)&&set_string(setDetail,o,detail);
 uint8_t raw=1;void*rawArgs[]={&raw};if(ok){m->mono_runtime_invoke(setRawTitle,o,rawArgs,&ex);ok=!ex;}
 void*args[]={m->mono_gchandle_get_target(h),NULL};if(ok){m->mono_runtime_invoke(addItem,manager,args,&ex);ok=!ex;}
 m->mono_gchandle_free(h);return ok;
}
// Polling/action revisions are not native-list revisions. Tokens identify the
// exact row/action context; unrelated backend changes never recreate controls.
static int same_page(const PmDeckFrame*a,const PmDeckFrame*b){
 if(a->page!=b->page||a->count!=b->count||strcmp(a->title,b->title)||strcmp(a->subtitle,b->subtitle))return 0;
 for(unsigned i=0;i<a->count;i++)if(memcmp(a->rows[i].token,b->rows[i].token,32)||a->rows[i].flags!=b->rows[i].flags||strcmp(a->rows[i].label,b->rows[i].label)||strcmp(a->rows[i].detail,b->rows[i].detail))return 0;
 return 1;
}
static unsigned focus_row(const PmDeckFrame*before,const PmDeckFrame*after,const char*id){
 unsigned old=before->count;
 if(id&&!strncmp(id,"prosperomenu.cheat.",sizeof "prosperomenu.cheat."-1)&&strlen(id+(sizeof "prosperomenu.cheat."-1))==32)
  for(unsigned i=0;i<before->count;i++)if(!memcmp(before->rows[i].token,id+(sizeof "prosperomenu.cheat."-1),32)){old=i;break;}
 if(old<before->count){
  // Keep the highlighted action even when a progress/status row is inserted.
  for(unsigned i=0;i<after->count;i++)if((after->rows[i].flags&1)&&!memcmp(before->rows[old].token,after->rows[i].token,32))return i;
  for(unsigned i=0;i<after->count;i++)if((after->rows[i].flags&1)&&!strcmp(before->rows[old].label,after->rows[i].label))return i;
 }
 unsigned from=old<after->count?old:0;
 for(unsigned n=0;n<after->count;n++){unsigned i=(from+n)%after->count;if(after->rows[i].flags&1)return i;}
 return after->count;
}
void pm_cheats_tick(void){
 if(!installed||!active||!appRoot||!managerRoot)return;
 // ShellUI can close a settings scene without dispatching the overridden
 // app.OnUnloaded method (AOT uses its own close path). UIManager.Active is
 // cleared by native OnUnloadEnqueue; observe that actual lifecycle state.
 if(now()>=checkAt){
  checkAt=now()+200;void*value=call(getActive,m->mono_gchandle_get_target(managerRoot),NULL);
  void*raw=value?m->mono_object_unbox(value):NULL;
  if(raw&&*(uint8_t*)raw)managerSeen=1;
  else if(raw&&(managerSeen||now()>managerDeadline)){
   __atomic_store_n(&active,0,__ATOMIC_RELEASE);clear_roots();if(trace)trace->cheatsStage=6;return;
  }
 }
 if(pthread_mutex_trylock(&guard))return;int change=refresh&&!actionPending;pthread_mutex_unlock(&guard);
 if(!change)return; // No managed allocations/lookups on idle render frames.
 void*manager=m->mono_gchandle_get_target(managerRoot),*id=m->mono_string_new(domain,"prosperomenu_info"),*lookup[]={id};
 if(!call(getElement,manager,lookup))return;
 char focused[64]={0};void*item=call(getFocused,manager,NULL);if(item)ascii(call(getId,item,NULL),focused,sizeof focused);
 if(pthread_mutex_trylock(&guard))return;change=refresh&&!actionPending;PmDeckFrame previous=shown;
 if(change){shown=incoming;refresh=0;make_xml(&shown,xml,sizeof xml);}pthread_mutex_unlock(&guard);
 if(!change)return;
 // ResetPage only rebuilds existing ElementData; it does not reread XML.
 // Rebuild only on actual content changes, preserving the current native focus.
 for(unsigned i=0;i<previous.count;i++){char name[64];snprintf(name,sizeof name,"prosperomenu.cheat.%.*s",32,previous.rows[i].token);void*str=m->mono_string_new(domain,name),*a[]={str};call(removeItem,manager,a);}
 void*info=m->mono_string_new(domain,"prosperomenu_info"),*remove[]={info};call(removeItem,manager,remove);
 int ok=add_row(manager,"prosperomenu_info",shown.subtitle,"",0);
 for(unsigned i=0;i<shown.count;i++){char name[64],title[112];const PmDeckRow*r=&shown.rows[i];snprintf(name,sizeof name,"prosperomenu.cheat.%.*s",32,r->token);snprintf(title,sizeof title,"%s%s",r->flags&2?"[ON] ":"",r->label);ok=add_row(manager,name,title,r->detail,r->flags&1)&&ok;}
 unsigned row=focus_row(&previous,&shown,focused);
 if(row<shown.count){char name[64];snprintf(name,sizeof name,"prosperomenu.cheat.%.*s",32,shown.rows[row].token);void*str=m->mono_string_new(domain,name),*a[]={str};call(focusItem,manager,a);}
 if(!ok&&trace)trace->cheatsError=6;
}
int pm_cheats_active(void){return __atomic_load_n(&active,__ATOMIC_ACQUIRE)||__atomic_load_n(&opening,__ATOMIC_ACQUIRE);}
void pm_cheats_poll(const char credential[33]){
 if(!installed)return;PmDeckFeedback feedback={0};pm_dashboard_feedback(&feedback);unsigned next=feedback.dashboardSerial;
 if(!next&&!pm_cheats_active()&&!actionPending)return;
 pthread_mutex_lock(&guard);PmDeckEvent event=action;unsigned pending=actionPending;pthread_mutex_unlock(&guard);
 // Complete an uncertain old-window action before accepting another title.
 // Otherwise an old row/sequence could accidentally target the new window.
 if(pending&&next&&next!=requestSerial){feedback.dashboardSerial=0;next=0;}
 PmDeckFrame frame;int status=pm_cheats_http(8780,credential,pending?&event:NULL,&feedback,&frame);
 // Definitive rejection ends this action; transport uncertainty retains its
 // original identity. Otherwise a stale row (409) would lock the page forever.
 pthread_mutex_lock(&guard);
 if(pending&&pm_deck_http_acknowledges(status)&&action.serial==event.serial)actionPending=0;
 pthread_mutex_unlock(&guard);
 if(!pm_deck_http_has_frame(status)){if(trace&&status>=400)trace->cheatsError=(unsigned)status;return;}
 pthread_mutex_lock(&guard);incoming=frame;
 int open=next&&next!=requestSerial&&frame.dashboard.ack==next;
 if(open){shown=frame;make_xml(&shown,xml,sizeof xml);requestSerial=next;refresh=0;}
 else if(active){
  if(same_page(&shown,&frame)){shown=frame;refresh=0;}
  else refresh=1;
 }
 pthread_mutex_unlock(&guard);
 if(open){pm_dashboard_ack(next);if(trace){trace->cheatsStage=1;trace->cheatsError=0;}
  // Native launch/foreground-user APIs are worker-only. UIManager.Push and all
  // managed objects remain on the system UI thread in StartDebugSettings.
  struct{uint32_t size;int user;}p={sizeof p,0};int code=foreground(&p.user);
  if(!code){initialize();launchDeadline=now()+10000;__atomic_store_n(&opening,1,__ATOMIC_RELEASE);code=launch("pssettings:play?mode=settings&function=debug_settings_old",&p);}
  if(code){__atomic_store_n(&opening,0,__ATOMIC_RELEASE);if(trace)trace->cheatsError=(unsigned)code;}else if(trace)trace->cheatsStage=2;
 }
 if(opening&&now()>launchDeadline){__atomic_store_n(&opening,0,__ATOMIC_RELEASE);if(trace)trace->cheatsError=7;}
}
void pm_cheats_bind(volatile PmDeckBoot*b){trace=b;installed=1;}
void pm_cheats_originals(void*t[4]){startOriginal=t[0];fileOriginal=t[1];pressOriginal=t[2];unloadOriginal=t[3];}
static void*symbol(const char*lib,const char*n){uint32_t h=0;return kernel_dynlib_handle(getpid(),lib,&h)?NULL:(void*)kernel_dynlib_dlsym(getpid(),h,n);}
int pm_cheats_prepare(PmMono*api,void*d,void*targets[4],void*replacements[4]){
 m=api;domain=d;if(!m->mono_string_length||!m->mono_string_chars)return 0;void*thread=m->mono_thread_attach(d);if(!thread)return 0;int ok=0;
 void*app=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI","SettingsApplicationUI3");
 void*manager=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI.Settings.CoreUI3","UIManager");
 void*element=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI.Settings.CoreUI3","SettingElement");
 void*handler=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI.Settings.DebugSettings","CategoryManager");
 void*encoding=klass("mscorlib","System.Text","Encoding");streamClass=klass("mscorlib","System.IO","MemoryStream");
 if(!app||!manager||!element||!handler||!encoding||!streamClass)goto done;
 managerField=m->mono_class_get_field_from_name(app,"uiManager");if(!managerField||m->mono_type_get_type(m->mono_field_get_type(managerField))!=0x12)goto done;
 const int strings[]={0xe,0xe,0x11},str[]={0xe},classes[]={0x12,0x12},streamTypes[]={0x1d,2};
 push=method(manager,"Push",3,1,strings,0);getId=method(element,"get_Id",0,0xe,NULL,0);
 encodingUtf8=method(encoding,"get_UTF8",0,0x12,NULL,1);getBytes=method(encoding,"GetBytes",1,0x1d,str,0);streamCtor=method(streamClass,".ctor",2,1,streamTypes,0);
 if(!push||!getId||!encodingUtf8||!getBytes||!streamCtor)goto done;
 void*data=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI.Settings.CoreUI3","ElementData");
 buttonClass=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI.Settings.CoreUI3","ButtonElementData");labelClass=klass("Sce.Vsh.ShellUI.Legacy","Sce.Vsh.ShellUI.Settings.CoreUI3","LabelElementData");
 const int itemTypes[]={0x12,0xe},boolType[]={2};
 addItem=method(manager,"AddMenuItem",2,1,itemTypes,0);removeItem=method(manager,"RemoveMenuItem",1,1,str,0);focusItem=method(manager,"FocusTo",1,1,str,0);getElement=method(manager,"GetElement",1,0x12,str,0);getFocused=method(manager,"GetFocusedMenuItem",0,0x12,NULL,0);getActive=method(manager,"get_Active",0,2,NULL,0);
 buttonCtor=method(buttonClass,".ctor",0,1,NULL,0);labelCtor=method(labelClass,".ctor",0,1,NULL,0);
 setId=method(data,"set_Id",1,1,str,0);setTitle=method(data,"set_Title",1,1,str,0);setDetail=method(data,"set_SecondTitle",1,1,str,0);setRawTitle=method(data,"set_RawTitle",1,1,boolType,0);
 if(!addItem||!removeItem||!focusItem||!getFocused||!getActive||!getElement||!buttonCtor||!labelCtor||!setId||!setTitle||!setDetail||!setRawTitle)goto done;
 void*sig=m->mono_method_signature(push),*pi=NULL;m->mono_signature_get_params(sig,&pi);m->mono_signature_get_params(sig,&pi);void*anim=m->mono_class_from_mono_type(m->mono_signature_get_params(sig,&pi));uint32_t alignment=0;
 if(!anim||!m->mono_class_is_enum(anim)||m->mono_class_value_size(anim,&alignment)!=4||alignment>4)goto done;
 void*methods[]={method(app,"StartDebugSettings",0,1,NULL,0),method(app,"ReadFromAssembly",1,0x12,str,0),method(handler,"OnPress",2,1,classes,0),method(app,"OnUnloaded",0,1,NULL,0)};
 for(unsigned i=0;i<4;i++){if(!methods[i]||!(targets[i]=m->mono_compile_method(methods[i])))goto done;}
 initialize=symbol("libSceShellUIUtil.sprx","sceShellUIUtilInitialize");launch=symbol("libSceShellUIUtil.sprx","sceShellUIUtilLaunchByUri");foreground=symbol("libSceUserService.sprx","sceUserServiceGetForegroundUser");if(!initialize||!launch||!foreground)goto done;
 replacements[0]=start;replacements[1]=read_file;replacements[2]=press;replacements[3]=unloaded;ok=1;
 done:m->mono_thread_detach(thread);return ok;
}
