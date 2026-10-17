// SPDX-License-Identifier: GPL-3.0-or-later
// PUI entry points informed by etaHEN / LightningMods' overlay-ui and MonoUtils.
// Original ProsperoMenu view, metadata validation and managed-object lifetime handling.
#include "deck-view.h"
#include "deck-layout-docked.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
static void* call(PmDeckView* v,void* method,void* object,void** args,int* ok){
 if(!*ok)return NULL;
 if(!method){*ok=0;return NULL;}void* exception=NULL;void* result=v->mono.mono_runtime_invoke(method,object,args,&exception);
 if(exception)*ok=0;return result;
}
static void* image(PmDeckView* v,const char* name){
 char path[192];snprintf(path,sizeof path,"/system_ex/common_ex/lib/%s",name);
 void* assembly=v->mono.mono_domain_assembly_open(v->domain,path);return assembly?v->mono.mono_assembly_get_image(assembly):NULL;
}
static int type(PmDeckView* v,void* t){return t?v->mono.mono_type_get_type(t):-1;}
static void* method(PmDeckView* v,void* klass,const char* name,unsigned count,int isStatic,int returnType,const int* parameters){
 void* iterator=NULL;void* found=NULL;void* m;
 while(klass&&(m=v->mono.mono_class_get_methods(klass,&iterator))){
  if(strcmp(v->mono.mono_method_get_name(m),name))continue;
  void* signature=v->mono.mono_method_signature(m);if(!signature||v->mono.mono_signature_get_param_count(signature)!=count)continue;
  uint32_t flags=v->mono.mono_method_get_flags(m,NULL);if(!!(flags&16)!=isStatic)continue;
  if(returnType>=0&&type(v,v->mono.mono_signature_get_return_type(signature))!=returnType)continue;
  void* it=NULL;unsigned i;for(i=0;i<count;i++)if(parameters){
   void* parameter=v->mono.mono_signature_get_params(signature,&it);int actual=type(v,parameter);
   if(parameters[i]==-2){
    if(actual==8||actual==9)continue;
    void* ec=v->mono.mono_class_from_mono_type(parameter);
    if(actual!=0x11||!ec||!v->mono.mono_class_is_enum(ec))break;
    int base=type(v,v->mono.mono_class_enum_basetype(ec));if(base!=8&&base!=9)break;
   }else if(actual!=parameters[i])break;
  }
  if(i!=count)continue;if(found)return NULL;found=m;
 }
 return found;
}
// Match the exact managed reference type: Widget has a second internal
// AppendChild overload with the same name, arity and return type on 13.60.
static void* object_method(PmDeckView* v,void* klass,const char* name,void* parameterClass){
 void* iterator=NULL;void* found=NULL;void* m;
 while(klass&&(m=v->mono.mono_class_get_methods(klass,&iterator))){
  if(strcmp(v->mono.mono_method_get_name(m),name)||(v->mono.mono_method_get_flags(m,NULL)&16))continue;
  void* sig=v->mono.mono_method_signature(m);void* it=NULL;
  if(!sig||v->mono.mono_signature_get_param_count(sig)!=1||type(v,v->mono.mono_signature_get_return_type(sig))!=1)continue;
  void* param=v->mono.mono_signature_get_params(sig,&it);
  if(type(v,param)!=0x12||v->mono.mono_class_from_mono_type(param)!=parameterClass)continue;
  if(found)return NULL;found=m;
 }
 return found;
}
static void* setter(PmDeckView* v,void* klass,const char* name,int valueType){
 for(unsigned depth=0;klass&&depth<12;depth++,klass=v->mono.mono_class_get_parent(klass)){
  void* property=v->mono.mono_class_get_property_from_name(klass,name);if(!property)continue;
  void* m=v->mono.mono_property_get_set_method(property);if(!m)return NULL;
  void* signature=v->mono.mono_method_signature(m);void* it=NULL;
  if((v->mono.mono_method_get_flags(m,NULL)&16)||!signature||v->mono.mono_signature_get_param_count(signature)!=1||type(v,v->mono.mono_signature_get_return_type(signature))!=1)return NULL;
  if(valueType>=0&&type(v,v->mono.mono_signature_get_params(signature,&it))!=valueType)return NULL;return m;
 }return NULL;
}
static void* value_setter(PmDeckView* v,void* klass,const char* name,void* expected){
 void* m=setter(v,klass,name,0x11);if(!m)return NULL;
 void* signature=v->mono.mono_method_signature(m);void* iterator=NULL;
 void* parameter=v->mono.mono_signature_get_params(signature,&iterator);
 return v->mono.mono_class_from_mono_type(parameter)==expected?m:NULL;
}
static int managed_styles(PmDeckView* v){
 // Both objects are allocated, constructed and unboxed by Mono. There is no
 // native color buffer or memcpy requiring a 16-byte RGBA layout. 13.60's
 // UIColor also contains an HDR flag; validate managed identity instead.
 return v->mono.mono_class_is_valuetype(v->font)&&v->mono.mono_class_is_valuetype(v->color);
}
static int pad_abi(PmDeckView* v,void* get){
 void* signature=v->mono.mono_method_signature(get);void* t=v->mono.mono_signature_get_return_type(signature);
 if(type(v,t)!=0x11)return 0;void* klass=v->mono.mono_class_from_mono_type(t);uint32_t align=0;
 if(!klass||!v->mono.mono_class_is_valuetype(klass)||v->mono.mono_class_value_size(klass,&align)!=36||align!=4)return 0;
 const char* names[]={"Skip","Buttons","ButtonsPrev","ButtonsDown","ButtonsUp","AnalogLeftX","AnalogLeftY","AnalogRightX","AnalogRightY"};
 const unsigned offsets[]={0,4,8,12,16,20,24,28,32};
 for(unsigned i=0;i<9;i++){
  void* field=v->mono.mono_class_get_field_from_name(klass,names[i]);
  if(!field||v->mono.mono_field_get_offset(field)!=16+offsets[i])return 0;
  void* ft=v->mono.mono_field_get_type(field);int actual=type(v,ft);
  if(i==0){if(actual!=2)return 0;}
  else if(i>=5){if(actual!=0x0c)return 0;}
  else{void* ec=v->mono.mono_class_from_mono_type(ft);
   if(actual!=0x11||!ec||!v->mono.mono_class_is_enum(ec)||type(v,v->mono.mono_class_enum_basetype(ec))!=9)return 0;}
 }
 return 1;
}
static void preparing(PmDeckView* v,uint32_t stage){v->prepareError=stage;if(v->prepareTrace)*v->prepareTrace=stage;}
int pm_deck_view_prepare(PmDeckView* v,void** update,void** input,volatile uint32_t* trace){
 memset(v,0,sizeof *v);v->prepareTrace=trace;*update=NULL;*input=NULL;
 preparing(v,101);if(!pm_mono_load(&v->mono))return 0;
 preparing(v,102);if((v->domain=v->mono.mono_get_root_domain())==NULL)return 0;
 preparing(v,103);
 void* thread=v->mono.mono_thread_attach(v->domain);if(!thread)return 0;int ok=0;
 void* pui=image(v,"Sce.PlayStation.PUI.dll"),*core=image(v,"Sce.PlayStation.Core.dll");
 void* appSystem=image(v,"Sce.Vsh.ShellUI.AppSystem.dll");
 void* layers=appSystem?v->mono.mono_class_from_name(appSystem,"Sce.Vsh.ShellUI.AppSystem","LayerManager"):NULL;
 v->gameLayerFocused=method(v,layers,"IsGameLayerFullScreenFocused",0,1,2,NULL);
 preparing(v,104);if(!pui||!core)goto done;
 // Record what PUI offers for images and text. Read-only; failure is ignored.
 pui_probe_run(&v->mono,v->domain,pui);
 v->label=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","Label");
 v->panel=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","Panel");
 v->widget=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","Widget");
 v->font=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","UIFont");
 v->color=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI","UIColor");
 void* application=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI","Application");
 void* gamepad=v->mono.mono_class_from_name(core,"Sce.PlayStation.Core.Input","GamePad");
 void* scene=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","Scene");
 preparing(v,105);if(!v->label||!v->panel||!v->widget||!v->font||!v->color||!application||!gamepad||!scene)goto done;
 void* parent=v->panel;unsigned depth=0;
 while(parent&&parent!=v->widget&&depth++<12)parent=v->mono.mono_class_get_parent(parent);
 preparing(v,106);if(parent!=v->widget)goto done;
 int i32[]={8},fontTypes[]={8,-2,-2},colorTypes[]={0x0c,0x0c,0x0c,0x0c};
 void* tick=method(v,application,"Update",0,0,1,NULL);
 void* get=method(v,gamepad,"GetData",1,1,0x11,i32);
 preparing(v,107);if(!tick||!get)goto done;
 preparing(v,108);if(!pad_abi(v,get))goto done;
 v->getApplication=method(v,application,"get_Instance",0,1,0x12,NULL);
 v->findScene=method(v,application,"get_RootScene",0,0,0x12,NULL);
 v->sceneClass=scene;
 v->sceneVisible=method(v,scene,"get_ActualVisible",0,0,2,NULL);
 v->containerClass=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","ContainerScene");
 void* sceneBase=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI","SceneBase");
 v->sceneCtor=method(v,scene,".ctor",0,0,1,NULL);
 v->addScene=object_method(v,v->containerClass,"AddScene",sceneBase);
 v->removeScene=object_method(v,v->containerClass,"RemoveScene",sceneBase);
 v->sceneOverlay=setter(v,scene,"ScreenRenderOverlay",2);
 v->sceneFocusable=setter(v,scene,"Focusable",2);v->sceneFrontable=setter(v,scene,"Frontable",2);
 v->scenePassFrontActive=setter(v,scene,"PassFrontActiveToNext",2);
 v->sceneWidth=setter(v,scene,"DesignWidth",0x0c);v->sceneHeight=setter(v,scene,"DesignHeight",0x0c);
 v->sceneBackground=setter(v,scene,"BackgroundSceneOpacity",0x0c);
 void* opacityControl=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","BackgroundOpacityControl");
 v->sceneBackgroundControl=value_setter(v,scene,"BackgroundOpacityControl",opacityControl);
 v->sceneBackgroundLimit=setter(v,scene,"BackgroundSceneOpacityUpperLimit",0x0c);
 void* property=v->mono.mono_class_get_property_from_name(scene,"RootWidget");
 v->getRoot=property?v->mono.mono_property_get_get_method(property):NULL;
 v->append=object_method(v,v->widget,"AppendChild",v->widget);
 v->remove=method(v,v->widget,"RemoveFromParent",0,0,1,NULL);
 v->labelCtor=method(v,v->label,".ctor",0,0,1,NULL);
 v->panelCtor=method(v,v->panel,".ctor",0,0,1,NULL);
 v->panelWidth=setter(v,v->panel,"Width",0x0c);v->panelHeight=setter(v,v->panel,"Height",0x0c);
 v->panelColor=value_setter(v,v->panel,"BackgroundColor",v->color);v->panelVisible=setter(v,v->panel,"Visible",2);
 v->fontCtor=method(v,v->font,".ctor",3,0,1,fontTypes);
 v->colorCtor=method(v,v->color,".ctor",4,0,1,colorTypes);
 preparing(v,120);
 // Optional image widget. UI2.ImageBox is a Widget sibling of Label and Panel
 // (parameterless ctor, LoadAsync(string), float Width/Height, bool Visible).
 // Resolved independently of the required ABI: a missing symbol only means the
 // replaceable logo falls back to the drawn mark.
 v->imageBox=v->mono.mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","ImageBox");
 if(v->imageBox){
  int imageString[]={0x0e};
  v->imageBoxCtor=method(v,v->imageBox,".ctor",0,0,1,NULL);
  v->imageWidth=setter(v,v->imageBox,"Width",0x0c);v->imageHeight=setter(v,v->imageBox,"Height",0x0c);
  v->imageVisible=setter(v,v->imageBox,"Visible",2);
  v->imageLoadAsync=method(v,v->imageBox,"LoadAsync",1,0,1,imageString);
 }
 v->text=setter(v,v->label,"Text",0x0e);v->name=setter(v,v->widget,"Name",0x0e);
 v->x=setter(v,v->widget,"X",0x0c);v->y=setter(v,v->widget,"Y",0x0c);
 v->width=setter(v,v->label,"FitWidthToText",2);v->height=setter(v,v->label,"FitHeightToText",2);
 v->textColor=value_setter(v,v->label,"TextColor",v->color);v->labelFont=value_setter(v,v->label,"Font",v->font);
 preparing(v,109);
 if(!managed_styles(v))goto done;
 preparing(v,110);if(!v->getApplication||!v->findScene||!v->sceneVisible)goto done;
 preparing(v,111);if(!v->getRoot)goto done;
 preparing(v,112);if(!v->append)goto done;
 preparing(v,113);if(!v->remove)goto done;
 preparing(v,114);if(!v->labelCtor)goto done;
 preparing(v,115);if(!v->fontCtor)goto done;
 preparing(v,116);if(!v->colorCtor)goto done;
 preparing(v,117);if(!v->text)goto done;
 preparing(v,118);if(!v->name)goto done;
 preparing(v,119);if(!v->x)goto done;
 preparing(v,120);if(!v->y)goto done;
 preparing(v,121);if(!v->width)goto done;
 preparing(v,122);if(!v->height)goto done;
 preparing(v,123);if(!v->textColor)goto done;
 preparing(v,124);if(!v->labelFont)goto done;
 preparing(v,125);if(!v->panelCtor)goto done;
 preparing(v,126);if(!v->panelWidth)goto done;
 preparing(v,127);if(!v->panelHeight)goto done;
 preparing(v,128);if(!v->panelColor)goto done;
 preparing(v,129);if(!v->panelVisible)goto done;
 void* colorSignature=v->mono.mono_method_signature(v->panelColor),*colorIterator=NULL;
 preparing(v,130);
 if(v->mono.mono_class_from_mono_type(v->mono.mono_signature_get_params(colorSignature,&colorIterator))!=v->color)goto done;
 preparing(v,132);if(!v->containerClass||!sceneBase||!v->sceneCtor||!v->addScene||!v->removeScene)goto done;
 preparing(v,133);if(!v->sceneOverlay||!v->sceneFocusable||!v->sceneFrontable||!v->sceneWidth||!v->sceneHeight||!v->sceneBackground)goto done;
 preparing(v,134);
 if(!opacityControl||!v->mono.mono_class_is_enum(opacityControl)||!v->sceneBackgroundControl||!v->sceneBackgroundLimit)goto done;
 int opacityBase=type(v,v->mono.mono_class_enum_basetype(opacityControl));
 if(opacityBase!=8&&opacityBase!=9)goto done;
 void* manualField=v->mono.mono_class_get_field_from_name(opacityControl,"Manual");
 void* manualValue=manualField?v->mono.mono_field_get_value_object(v->domain,manualField,NULL):NULL;
 if(!manualValue||v->mono.mono_object_get_class(manualValue)!=opacityControl)goto done;
 void* manualData=v->mono.mono_object_unbox(manualValue);if(!manualData)goto done;
 memcpy(&v->sceneManualMode,manualData,sizeof v->sceneManualMode);
 preparing(v,135);if(!v->scenePassFrontActive)goto done;
 preparing(v,131);
 *update=v->mono.mono_compile_method(tick);*input=v->mono.mono_compile_method(get);ok=*update&&*input;
done:if(ok){preparing(v,0);v->animate=1;}v->mono.mono_thread_detach(thread);return ok;
}
static void* target(PmDeckView* v,uint32_t h){return h?v->mono.mono_gchandle_get_target(h):NULL;}
static uint32_t object(PmDeckView* v,void* klass){void* o=v->mono.mono_object_new(v->domain,klass);return o?v->mono.mono_gchandle_new(o,1):0;}
static void set(PmDeckView* v,void* method,void* o,void* value,int* ok){void* args[]={value};call(v,method,o,args,ok);}
static void set_text(PmDeckView* v,uint32_t label,const char* text,int* ok){
 void* o=target(v,label);if(!o){*ok=0;return;}void* s=v->mono.mono_string_new(v->domain,text);if(!s){*ok=0;return;}set(v,v->text,o,s,ok);
}
static int styles(PmDeckView* v){
 int ok=1;
 for(unsigned i=0;i<PM_DECK_STYLES&&ok;i++){
  v->fonts[i]=object(v,v->font);if(!v->fonts[i])return 0;
  int size=pm_deck_font_sizes[i],style=0,weight=0;void* fa[]={&size,&style,&weight};
  call(v,v->fontCtor,v->mono.mono_object_unbox(target(v,v->fonts[i])),fa,&ok);
 }
 for(unsigned i=0;i<PM_DECK_INKS&&ok;i++){
  v->colors[i]=object(v,v->color);if(!v->colors[i])return 0;
  float r=pm_deck_ink[i][0],g=pm_deck_ink[i][1],b=pm_deck_ink[i][2],a=pm_deck_ink[i][3];
  void* ca[]={&r,&g,&b,&a};call(v,v->colorCtor,v->mono.mono_object_unbox(target(v,v->colors[i])),ca,&ok);
 }
 for(unsigned i=0;i<PM_DECK_BACKGROUNDS&&ok;i++){
  v->backgrounds[i]=object(v,v->color);if(!v->backgrounds[i])return 0;
  float r=pm_deck_background[i][0],g=pm_deck_background[i][1],b=pm_deck_background[i][2],a=pm_deck_background[i][3];
  void* ca[]={&r,&g,&b,&a};call(v,v->colorCtor,v->mono.mono_object_unbox(target(v,v->backgrounds[i])),ca,&ok);
 }
 return ok;
}
// ProsperoSettings theme: rewrite the existing UIColor values in place (no new
// managed objects) and repaint every widget once. Serial 0 is the built-in
// palette. Which shared ink/background slots each theme colour drives:
static const struct { uint8_t theme,ink,background; } pm_theme_slots[]={
 {PM_THEME_ACCENT,PM_INK_TEAL,PM_BG_TEAL},{PM_THEME_BACKGROUND,255,PM_BG_DOCK},
 {PM_THEME_TEXT,PM_INK_TEXT,255},{PM_THEME_MUTED,PM_INK_MUTED,255},
 {PM_THEME_SEPARATOR,255,PM_BG_SEPARATOR},{PM_THEME_SCROLLBAR,255,PM_BG_SCROLL},
 {PM_THEME_EDGE,255,PM_BG_EDGE},{PM_THEME_FOCUS_FILL,255,4},
 {PM_THEME_TITLE,PM_INK_TITLE,255},{PM_THEME_LOGO,255,PM_BG_LOGO}};
static int recolor(PmDeckView* v,uint32_t handle,const float rgba[4]){
 int ok=1;void* o=target(v,handle);if(!o)return 0;
 float r=rgba[0],g=rgba[1],b=rgba[2],a=rgba[3];void* ca[]={&r,&g,&b,&a};
 call(v,v->colorCtor,v->mono.mono_object_unbox(o),ca,&ok);return ok;
}
static int pm_theme_apply(PmDeckView* v,const PmDeckTheme* theme){
 int ok=1;
 for(unsigned i=0;i<sizeof pm_theme_slots/sizeof pm_theme_slots[0]&&ok;i++){
  uint32_t c=theme->rgba[pm_theme_slots[i].theme];unsigned ink=pm_theme_slots[i].ink,bg=pm_theme_slots[i].background;
  float custom[4]={((c>>24)&255)/255.0f,((c>>16)&255)/255.0f,((c>>8)&255)/255.0f,(c&255)/255.0f};
  if(ink<PM_DECK_INKS)ok=recolor(v,v->colors[ink],theme->serial?custom:pm_deck_ink[ink]);
  if(ok&&bg<PM_DECK_BACKGROUNDS)ok=recolor(v,v->backgrounds[bg],theme->serial?custom:pm_deck_background[bg]);
 }
 if(ok){v->themeSerial=theme->serial;v->paintedValid=0;}
 return ok;
}
static void release_styles(PmDeckView* v){
 for(unsigned i=0;i<PM_DECK_STYLES;i++){
  if(v->fonts[i])v->mono.mono_gchandle_free(v->fonts[i]);v->fonts[i]=0;
 }
 for(unsigned i=0;i<PM_DECK_INKS;i++){
  if(v->colors[i])v->mono.mono_gchandle_free(v->colors[i]);v->colors[i]=0;
 }
 for(unsigned i=0;i<PM_DECK_BACKGROUNDS;i++){
  if(v->backgrounds[i])v->mono.mono_gchandle_free(v->backgrounds[i]);v->backgrounds[i]=0;
 }
 v->initialized=0;
}
static int release_labels(PmDeckView* v){
 int complete=1;
 for(unsigned i=0;i<PM_DECK_LABELS;i++)if(v->labels[i]){
  int ok=1;
  if(v->attachedLabels&(UINT64_C(1)<<i)){
   set_text(v,v->labels[i],"",&ok);ok=1; // Try removal even if hiding failed.
   call(v,v->remove,target(v,v->labels[i]),NULL,&ok);
  }
  if(!ok){complete=0;continue;}
  v->mono.mono_gchandle_free(v->labels[i]);v->labels[i]=0;v->attachedLabels&=~(UINT64_C(1)<<i);
 }
 for(unsigned i=0;i<PM_DECK_PANELS;i++)if(v->panels[i]){
  int ok=1;
  if(v->attachedPanels&(1u<<i)){
   uint8_t hidden=0;set(v,v->panelVisible,target(v,v->panels[i]),&hidden,&ok);ok=1;
   call(v,v->remove,target(v,v->panels[i]),NULL,&ok);
  }
  if(!ok){complete=0;continue;}
  v->mono.mono_gchandle_free(v->panels[i]);v->panels[i]=0;v->attachedPanels&=~(1u<<i);
 }
 for(unsigned i=0;i<PM_DECK_IMAGES;i++)if(v->images[i]){
  int ok=1;
  if(v->attachedImages&(1u<<i)){
   uint8_t hidden=0;set(v,v->imageVisible,target(v,v->images[i]),&hidden,&ok);ok=1;
   call(v,v->remove,target(v,v->images[i]),NULL,&ok);
  }
  if(!ok){complete=0;continue;}
  v->mono.mono_gchandle_free(v->images[i]);v->images[i]=0;v->attachedImages&=~(1u<<i);
 }
 v->logoApplied[0]=0;
 if(complete&&v->root){v->mono.mono_gchandle_free(v->root);v->root=0;}
 v->paintedValid=0;return complete;
}
static int release_scene(PmDeckView* v){
 if(v->overlayScene&&v->sceneAttached){
  int ok=1;void* args[]={target(v,v->overlayScene)};
  call(v,v->removeScene,target(v,v->parentScene),args,&ok);if(!ok)return 0;
 }
 if(v->overlayScene)v->mono.mono_gchandle_free(v->overlayScene);
 if(v->parentScene)v->mono.mono_gchandle_free(v->parentScene);
 v->overlayScene=v->parentScene=0;v->sceneAttached=0;return 1;
}
static int create_scene(PmDeckView* v,void* parent){
 v->parentScene=v->mono.mono_gchandle_new(parent,1);if(!v->parentScene)return 0;
 v->overlayScene=object(v,v->sceneClass);void* scene=target(v,v->overlayScene);if(!scene)return 0;
 int ok=1;call(v,v->sceneCtor,scene,NULL,&ok);
 uint8_t yes=1,no=0;float width=1920,height=1080,background=0;
 // Own scene only: do not change the dashboard's visibility, focus or root.
 // Transparent/non-focusable scene keeps the normal dashboard underneath.
 set(v,v->sceneOverlay,scene,&yes,&ok);
 set(v,v->sceneFocusable,scene,&no,&ok);set(v,v->sceneFrontable,scene,&no,&ok);
 // Front activation is separate from input focus. Pass it to the dashboard.
 set(v,v->scenePassFrontActive,scene,&yes,&ok);
 set(v,v->sceneWidth,scene,&width,&ok);set(v,v->sceneHeight,scene,&height,&ok);
 // PUI accumulates BackgroundSceneOpacity from front to back, then subtracts
 // ForegroundSceneOpacity in both UI2 and UI3 RenderWidgets. This is occlusion,
 // not the desired alpha of the dashboard: 1 hides it; 0 leaves it visible.
 // Manual prevents automatic recomputation; zero upper limit also prevents
 // this scene from contributing occlusion during transitions.
 uint32_t manual=v->sceneManualMode;set(v,v->sceneBackgroundControl,scene,&manual,&ok);
 set(v,v->sceneBackgroundLimit,scene,&background,&ok);
 set(v,v->sceneBackground,scene,&background,&ok);
 if(ok){void* args[]={scene};v->sceneAttached=1;call(v,v->addScene,target(v,v->parentScene),args,&ok);}
 return ok;
}
static int hold_cleanup(PmDeckView* v,uint64_t now){v->failed=1;v->cleanupAt=now;return 0;}
static int fail_view(PmDeckView* v,uint64_t now){
 hold_cleanup(v,now);
 if(release_labels(v)&&release_scene(v))release_styles(v);
 return 0;
}
int pm_deck_view_game_focus(PmDeckView* v){
 if(!v->gameLayerFocused)return -2;
 int ok=1;void* boxed=call(v,v->gameLayerFocused,NULL,NULL,&ok);
 uint8_t* value=ok&&boxed?v->mono.mono_object_unbox(boxed):NULL;
 return value?!!*value:-1;
}
static int labels(PmDeckView* v,uint32_t root){
 v->root=root;if(!v->root)return 0;int ok=1;
 PmDeckFrame empty={0};PmDeckLayout layout;pm_deck_layout_classic(&empty,0,0,&layout);
 // Backgrounds are appended before labels. Create them hidden to avoid a flash
 // of partially constructed UI; visibility is published with the first frame.
 for(unsigned i=0;i<PM_DECK_PANELS&&ok;i++){
  if(v->hud&&(i<9||i>11))continue;
  v->panels[i]=object(v,v->panel);void* p=target(v,v->panels[i]);if(!p)return 0;
  call(v,v->panelCtor,p,NULL,&ok);char name[48];snprintf(name,sizeof name,"ProsperoMenuPanel_%u",i);
  set(v,v->name,p,v->mono.mono_string_new(v->domain,name),&ok);uint8_t hidden=0;
  set(v,v->panelVisible,p,&hidden,&ok);void* args[]={p};
  if(ok){v->attachedPanels|=1u<<i;call(v,v->append,target(v,v->root),args,&ok);}
 }
 for(unsigned i=0;i<PM_DECK_LABELS&&ok;i++){
  if(v->hud&&(i<41||i>44))continue;
  v->labels[i]=object(v,v->label);void* label=target(v,v->labels[i]);if(!label)return 0;
  call(v,v->labelCtor,label,NULL,&ok);char name[48];snprintf(name,sizeof name,"ProsperoMenuDeck_%u",i);
  set(v,v->name,label,v->mono.mono_string_new(v->domain,name),&ok);
  const PmDeckText* item=&layout.labels[i];float x=item->x,y=item->y;
  set(v,v->x,label,&x,&ok);set(v,v->y,label,&y,&ok);uint8_t yes=1;
  set(v,v->width,label,&yes,&ok);set(v,v->height,label,&yes,&ok);
  set(v,v->labelFont,label,v->mono.mono_object_unbox(target(v,v->fonts[item->font])),&ok);
  set(v,v->textColor,label,v->mono.mono_object_unbox(target(v,v->colors[item->ink])),&ok);
  set_text(v,v->labels[i],"",&ok);void* args[]={label};
  if(ok){v->attachedLabels|=UINT64_C(1)<<i;call(v,v->append,target(v,v->root),args,&ok);}
 }
 for(unsigned i=0;i<PM_DECK_IMAGES&&ok;i++){
  if(!v->imageBox||!v->imageBoxCtor||!v->imageLoadAsync||!v->imageWidth||!v->imageHeight||!v->imageVisible)break;
  v->images[i]=object(v,v->imageBox);void* picture=target(v,v->images[i]);if(!picture)return 0;
  call(v,v->imageBoxCtor,picture,NULL,&ok);char name[48];snprintf(name,sizeof name,"ProsperoMenuImage_%u",i);
  set(v,v->name,picture,v->mono.mono_string_new(v->domain,name),&ok);
  const PmDeckImage* slot=&layout.images[i];
  float px=slot->x,py=slot->y,pw=slot->width,ph=slot->height;
  set(v,v->x,picture,&px,&ok);set(v,v->y,picture,&py,&ok);
  set(v,v->imageWidth,picture,&pw,&ok);set(v,v->imageHeight,picture,&ph,&ok);
  uint8_t hidden=0;set(v,v->imageVisible,picture,&hidden,&ok);void* args[]={picture};
  if(ok){v->attachedImages|=1u<<i;call(v,v->append,target(v,v->root),args,&ok);}
 }
 return ok;
}
int pm_deck_view_render(PmDeckView* v,const PmDeckFrame* f,int open,unsigned selected,uint64_t now){
 if(v->failed){
  // A transient RemoveFromParent failure must not orphan a visible backplate.
  // Keep roots and retry only cleanup at most four times per second. Input
  // remains disabled; do not recreate a scene or resume normal rendering.
  if((v->root||v->overlayScene||v->parentScene)&&(now<v->cleanupAt||now-v->cleanupAt>=250)){
   v->cleanupAt=now;if(release_labels(v)&&release_scene(v))release_styles(v);
  }
  return 0;
 }
 // A closed full-screen menu must not remain in ShellUI's scene tree. Keep
 // the controller chord available without presenting an empty scene over the
 // dashboard. Closed-state HUD rendering needs an independent overlay host.
 int hudRequested=v->hud&&(f->flags&1)&&((f->flags&2&&f->ip[0])||f->metrics[0]||(f->notice.kind&&f->notice.remainingMs));
 if(!open&&!hudRequested){
  if(!release_labels(v)||!release_scene(v))return hold_cleanup(v,now);
  v->open=0;v->openingAt=0;v->sceneStatus=8;memset(&v->motion,0,sizeof v->motion);v->moving=0;return 1;
 }
 if(!v->openingAt)v->openingAt=now;
 int ok=1;
 if(!v->initialized){if(!styles(v))return fail_view(v,now);v->initialized=1;v->themeSerial=0;}
 if(f->theme.serial!=v->themeSerial&&!pm_theme_apply(v,&f->theme))return fail_view(v,now);
 if(!v->root||now-v->checkedAt>=250){
  v->checkedAt=now;
  // UI3 foreground scenes cannot host UI2 widgets. Keep a dedicated UI2 scene
  // under ShellUI's verified UI2 root container; never borrow the hidden Game
  // layer or change/replace the application's own root scene.
  void* app=call(v,v->getApplication,NULL,NULL,&ok);
  void* scene=app&&ok?call(v,v->findScene,app,NULL,&ok):NULL;
  void* klass=scene?v->mono.mono_object_get_class(scene):NULL;
  unsigned depth=0;while(klass&&klass!=v->containerClass&&depth++<32)klass=v->mono.mono_class_get_parent(klass);
  if(!ok)return fail_view(v,now);
  if(!scene||klass!=v->containerClass){
   v->sceneStatus=!app?1:!scene?2:3;
   if(!release_labels(v)||!release_scene(v))return hold_cleanup(v,now);return 0;
  }
  if(scene!=target(v,v->parentScene)){
   // Pin the replacement across cleanup and the managed calls it can trigger.
   uint32_t pinned=v->mono.mono_gchandle_new(scene,1);if(!pinned)return 0;
   if(!release_labels(v)||!release_scene(v)){v->mono.mono_gchandle_free(pinned);return hold_cleanup(v,now);}
   int created=create_scene(v,target(v,pinned));v->mono.mono_gchandle_free(pinned);
   if(!created)return fail_view(v,now);v->openingAt=now;
  }
  void* visible=call(v,v->sceneVisible,target(v,v->overlayScene),NULL,&ok);
  void* value=visible&&ok?v->mono.mono_object_unbox(visible):NULL;
  void* root=value&&*(uint8_t*)value?call(v,v->getRoot,target(v,v->overlayScene),NULL,&ok):NULL;
  v->sceneStatus=!ok?7:!value||!*(uint8_t*)value?4:!root?5:6;
  if(!ok)return fail_view(v,now);if(!root){
   if(!release_labels(v))return hold_cleanup(v,now);
   // Scene activation can take another UI tick. Do not close the pending menu
   // immediately, otherwise opening would endlessly create and remove it.
   if(now<v->openingAt||now-v->openingAt>=2000)return fail_view(v,now);
   v->sceneStatus=9;return 2;
  }
  if(root!=target(v,v->root)){
   uint32_t pinned=v->mono.mono_gchandle_new(root,1);if(!pinned)return 0;
   if(!release_labels(v)){v->mono.mono_gchandle_free(pinned);return hold_cleanup(v,now);}
   if(!labels(v,pinned))return fail_view(v,now);v->sequence=0;v->metricsSequence=0;v->noticeSequence=0;
  }
 }
 int menuChanged=!v->paintedValid||v->sequence!=f->sequence||v->selected!=selected||v->open!=open||v->flags!=f->flags;
 int noticeVisible=!open&&(f->flags&1)&&f->notice.kind&&f->notice.remainingMs;
 int noticeChanged=v->noticeSequence!=f->notice.sequence||v->noticeVisible!=!!noticeVisible;
 if(!menuChanged&&v->metricsSequence==f->metricsSequence&&!noticeChanged&&!v->moving)return 1;
 if(v->moving&&!menuChanged&&v->metricsSequence==f->metricsSequence&&!noticeChanged&&now>=v->motion.last&&now-v->motion.last<33)return 1;
 // The marquee phase runs from the moment the focused row changed. Reduce
 // motion (bit 6) keeps text still, showing the truncated form.
 if(v->selected!=selected||!v->paintedValid)v->selectedAt=now;
 uint64_t marqueeMs=(f->flags&64)?0:(now>=v->selectedAt?now-v->selectedAt:0);
 (void)marqueeMs; // the classic layout ignores the phase
 PmDeckLayout layout;pm_deck_layout(f,open,selected,&layout,marqueeMs);
 if(v->animate&&!(f->flags&64))v->moving=pm_deck_motion(&v->motion,&layout,selected,now)||layout.marquee;
 else {v->moving=0;memset(&v->motion,0,sizeof v->motion);}
 for(unsigned i=0;i<PM_DECK_LABELS&&ok;i++){
  if(v->hud&&(i<41||i>44))continue;
  const PmDeckText* current=&layout.labels[i];const PmDeckText* previous=&v->painted.labels[i];
  if(!v->paintedValid||current->x!=previous->x){float x=current->x;set(v,v->x,target(v,v->labels[i]),&x,&ok);}
  if(!v->paintedValid||current->y!=previous->y){float y=current->y;set(v,v->y,target(v,v->labels[i]),&y,&ok);}
  if(!v->paintedValid||strcmp(current->text,previous->text))set_text(v,v->labels[i],current->text,&ok);
  if(!v->paintedValid||current->font!=previous->font)set(v,v->labelFont,target(v,v->labels[i]),v->mono.mono_object_unbox(target(v,v->fonts[current->font])),&ok);
  if(!v->paintedValid||current->ink!=previous->ink){
   void* color=v->mono.mono_object_unbox(target(v,v->colors[current->ink]));
   set(v,v->textColor,target(v,v->labels[i]),color,&ok);
  }
 }
 for(unsigned i=0;i<PM_DECK_PANELS&&ok;i++){
  if(v->hud&&(i<9||i>11))continue;
  const PmDeckPanel* current=&layout.panels[i];const PmDeckPanel* previous=&v->painted.panels[i];void* p=target(v,v->panels[i]);
  if(!p){ok=0;break;}
  if(!v->paintedValid||current->x!=previous->x){float x=current->x;set(v,v->x,p,&x,&ok);}
  if(!v->paintedValid||current->y!=previous->y){float y=current->y;set(v,v->y,p,&y,&ok);}
  if(!v->paintedValid||current->width!=previous->width){float width=current->width;set(v,v->panelWidth,p,&width,&ok);}
  if(!v->paintedValid||current->height!=previous->height){float height=current->height;set(v,v->panelHeight,p,&height,&ok);}
  if(!v->paintedValid||current->ink!=previous->ink)set(v,v->panelColor,p,v->mono.mono_object_unbox(target(v,v->backgrounds[current->ink])),&ok);
  if(!v->paintedValid||current->visible!=previous->visible){uint8_t visible=(uint8_t)current->visible;set(v,v->panelVisible,p,&visible,&ok);}
 }
 // The replaceable logo: geometry from the layout, and the picture itself
 // loaded once per path change. A failed load leaves the logo hidden.
 for(unsigned i=0;i<PM_DECK_IMAGES&&ok;i++){
  const PmDeckImage* current=&layout.images[i];const PmDeckImage* previous=&v->painted.images[i];
  void* picture=target(v,v->images[i]);if(!picture||!(v->attachedImages&(1u<<i)))continue;
  if(!v->paintedValid||current->x!=previous->x){float px=current->x;set(v,v->x,picture,&px,&ok);}
  if(!v->paintedValid||current->y!=previous->y){float py=current->y;set(v,v->y,picture,&py,&ok);}
  if(!v->paintedValid||current->width!=previous->width){float pw=current->width;set(v,v->imageWidth,picture,&pw,&ok);}
  if(!v->paintedValid||current->height!=previous->height){float ph=current->height;set(v,v->imageHeight,picture,&ph,&ok);}
  if(current->visible&&(strcmp(v->logoApplied,current->path)||f->videoSerial!=v->videoSerialApplied)){
   void* argument=v->mono.mono_string_new(v->domain,current->path);void* load[]={argument};
   call(v,v->imageLoadAsync,picture,load,&ok);
   snprintf(v->logoApplied,sizeof v->logoApplied,"%s",ok?current->path:"");
   if(ok)v->videoSerialApplied=f->videoSerial;
  }
  if(!v->paintedValid||current->visible!=previous->visible){uint8_t visible=(uint8_t)current->visible;set(v,v->imageVisible,picture,&visible,&ok);}
 }
 if(!ok)return fail_view(v,now);
 v->painted=layout;v->paintedValid=1;
 v->sequence=f->sequence;v->metricsSequence=f->metricsSequence;v->noticeSequence=f->notice.sequence;v->noticeVisible=!!noticeVisible;
 v->selected=selected;v->open=open;v->flags=f->flags;return 1;
}
