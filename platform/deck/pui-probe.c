// SPDX-License-Identifier: GPL-3.0-or-later
// PUI member probe. Runs on the deck's prepare path, inside ShellUI where Mono
// is loaded, and records what Sce.PlayStation.PUI exposes for images and text so
// a validated logo/image path can be built from real evidence instead of guessed
// metadata. Read-only reflection: it creates no managed object and draws nothing.
//
// Output: /data/ProsperoMenu/pui-probe.txt (copy it out over FTP).
#include "mono-api.h"
#ifdef __PS5__
#include <ps5/klog.h>
#define PUI_PROBE_LOG(...) klog_printf(__VA_ARGS__)
#else
#define PUI_PROBE_LOG(...) ((void)0) // host builds have no klog channel
#endif
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define PUI_REPORT "/data/ProsperoMenu/pui-probe.txt"
#define PROBE_CAP 64000u
#define MONO_TABLE_TYPEDEF 0x02
#define TYPE_TOKEN(row) (0x02000000u | (uint32_t)(row))

static char report[PROBE_CAP]; static size_t used;
static void put(const char* s){size_t n=strlen(s);if(used+n+1>=PROBE_CAP)return;memcpy(report+used,s,n);used+=n;report[used]=0;}
static void putf(const char* fmt,...){char b[320];va_list a;va_start(a,fmt);vsnprintf(b,sizeof b,fmt,a);va_end(a);put(b);}
static const char* s_or(const char* s,const char* fallback){return s?s:fallback;}

static void type_text(PmMono* m,void* type,char* out,size_t n){
 const char* name=type&&m->mono_type_get_name?m->mono_type_get_name(type):NULL;
 if(name){snprintf(out,n,"%s",name);return;}
 snprintf(out,n,"type#%d",type&&m->mono_type_get_type?m->mono_type_get_type(type):-1);
}
// Names worth dumping in full: PUI types that are image-, text- or colour-shaped.
static int interesting(const char* ns,const char* name){
 static const char* keys[]={"Image","Texture","Sprite","Picture","Icon","Brush","Canvas",
   "Material","Plane","Quad","Render","Bitmap","Blend","Shape","Atlas","Font","Color","Text","Label","Widget","Panel"};
 if(!name||!ns||!strstr(ns,"PUI"))return 0;
 for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++)if(strstr(name,keys[i]))return 1;
 return 0;
}
static void dump_class(PmMono* m,const char* ns,const char* name,void* k){
 if(!k)return;
 putf("TYPE %s.%s\n",s_or(ns,""),s_or(name,""));
 void* parent=k;
 for(unsigned d=0;d<8;d++){
  parent=m->mono_class_get_parent(parent);if(!parent)break;
  putf("  base %s.%s\n",
    s_or(m->mono_class_get_namespace?m->mono_class_get_namespace(parent):NULL,""),
    s_or(m->mono_class_get_name?m->mono_class_get_name(parent):NULL,""));
 }
 void* it=NULL,*meth;unsigned n=0;
 while((meth=m->mono_class_get_methods(k,&it))&&n<160){
  const char* mn=m->mono_method_get_name(meth);void* sig=m->mono_method_signature(meth);
  unsigned pc=sig?m->mono_signature_get_param_count(sig):0;uint32_t flags=m->mono_method_get_flags(meth,NULL);
  putf("  m %s(%u)%s",s_or(mn,"?"),pc,(flags&0x10)?" static":"");
  if(sig&&pc){void* pi=NULL;for(unsigned i=0;i<pc;i++){void* pt=m->mono_signature_get_params(sig,&pi);char tb[160];type_text(m,pt,tb,sizeof tb);putf(" %s",tb);}}
  put("\n");n++;
 }
 if(m->mono_class_get_properties){
  void* pit=NULL,*prop;unsigned q=0;
  while((prop=m->mono_class_get_properties(k,&pit))&&q<160){
   const char* pn=m->mono_property_get_name?m->mono_property_get_name(prop):NULL;
   void* get=m->mono_property_get_get_method(prop),*set=m->mono_property_get_set_method(prop);
   putf("  p %s get=%d set=%d",s_or(pn,"?"),get?1:0,set?1:0);
   if(set){void* s=m->mono_method_signature(set);
    if(s&&m->mono_signature_get_param_count(s)==1){void* pi=NULL;void* pt=m->mono_signature_get_params(s,&pi);char tb[160];type_text(m,pt,tb,sizeof tb);putf(" setType=%s",tb);}}
   put("\n");q++;
  }
 }
}
void pui_probe_run(PmMono* m,void* domain,void* puiImage){
 (void)domain;
 if(!m||!puiImage)return;
 used=0;report[0]=0;
 put("ProsperoMenu PUI member probe\n");
 putf("symbols tables=%d decode=%d heap=%d class_get=%d props=%d typename=%d\n",
  !!m->mono_image_get_table_info,!!m->mono_metadata_decode_row,!!m->mono_metadata_string_heap,
  !!m->mono_class_get,!!m->mono_class_get_properties,!!m->mono_type_get_name);
 int rows=0;
 void* table=m->mono_image_get_table_info?m->mono_image_get_table_info(puiImage,MONO_TABLE_TYPEDEF,&rows):NULL;
 putf("typedef rows=%d table=%d\n",rows,table?1:0);
 unsigned dumped=0;
 if(table&&m->mono_metadata_decode_row&&m->mono_metadata_string_heap&&m->mono_class_get){
  for(int r=0;r<rows&&dumped<120;r++){
   uint32_t data[4]={0,0,0,0};
   m->mono_metadata_decode_row(table,r,data,4);
   const char* name=m->mono_metadata_string_heap(puiImage,data[1]);
   const char* ns=m->mono_metadata_string_heap(puiImage,data[2]);
   if(!interesting(ns,name))continue;
   void* k=m->mono_class_get(puiImage,TYPE_TOKEN(r));
   if(!k)continue;
   dump_class(m,ns,name,k);dumped++;
  }
 } else {
  static const char* names[]={"Panel","Label","Widget","UIFont","UIColor","Scene","Application","Image","Texture","Sprite","Picture"};
  static const char* nss[]={"Sce.PlayStation.PUI.UI2","Sce.PlayStation.PUI"};
  for(unsigned i=0;i<sizeof names/sizeof names[0];i++)for(unsigned j=0;j<sizeof nss/sizeof nss[0];j++){
   void* k=m->mono_class_from_name(puiImage,nss[j],names[i]);
   if(k){dump_class(m,nss[j],names[i],k);dumped++;}
  }
 }
 putf("dumped=%u\n",dumped);
 // klog is the reliable readout: ShellUI's own filesystem view may not allow
 // writing /data, but the system log always accepts lines. Read with the
 // devloop klog client and filter on "PMPROBE".
 PUI_PROBE_LOG("PMPROBE begin\n");
 {
  char line[200];size_t at=0;
  for(const char* q=report;;q++){
   if(*q=='\n'||!*q){
    line[at]=0;if(at)PUI_PROBE_LOG("PMPROBE %s\n",line);
    at=0;if(!*q)break;
   }else if(at<sizeof line-1)line[at++]=*q;
  }
 }
 PUI_PROBE_LOG("PMPROBE end\n");
 int fd=open(PUI_REPORT,O_WRONLY|O_CREAT|O_TRUNC,0666);
 if(fd>=0){size_t at=0;while(at<used){ssize_t w=write(fd,report+at,used-at);if(w<=0)break;at+=(size_t)w;}close(fd);}
}
