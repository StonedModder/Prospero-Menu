// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "loader-worker.h"
#include <errno.h>
#include <stdio.h>
static JSValue error(JSContext *c){
  JSValue value=JS_NewError(c);JS_SetPropertyStr(c,value,"code",JS_NewString(c,errno==EBUSY?"LOADER_BUSY":"LOADER_NATIVE"));
  JS_SetPropertyStr(c,value,"message",JS_NewString(c,"Native payload dispatch could not be started or inspected"));return JS_Throw(c,value);
}
static JSValue start(JSContext *c,JSValueConst self,int argc,JSValueConst *args){
  (void)self;if(argc!=3)return JS_ThrowTypeError(c,"Expected request, upload and checksum");
  const char *request=JS_ToCString(c,args[0]),*upload=JS_ToCString(c,args[1]),*hash=JS_ToCString(c,args[2]);
  int result=-1;if(request&&upload&&hash)result=pm_loader_start(request,upload,hash);
  if(request)JS_FreeCString(c,request);if(upload)JS_FreeCString(c,upload);if(hash)JS_FreeCString(c,hash);
  return result?error(c):JS_UNDEFINED;
}
static JSValue plugin_start(JSContext *c,JSValueConst self,int argc,JSValueConst *args){
  (void)self;if(argc!=3)return JS_ThrowTypeError(c,"Expected request, upload and checksum");
  const char *request=JS_ToCString(c,args[0]),*upload=JS_ToCString(c,args[1]),*hash=JS_ToCString(c,args[2]);
  int result=-1;if(request&&upload&&hash)result=pm_plugin_start(request,upload,hash);
  if(request)JS_FreeCString(c,request);if(upload)JS_FreeCString(c,upload);if(hash)JS_FreeCString(c,hash);
  return result?error(c):JS_UNDEFINED;
}
static JSValue poll(JSContext *c,JSValueConst self,int argc,JSValueConst *args){
  (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected dispatch request");
  const char *request=JS_ToCString(c,args[0]);if(!request)return JS_EXCEPTION;
  PmDispatchReceipt r;int result=pm_loader_poll(request,&r);JS_FreeCString(c,request);
  if(result)return errno==ENOENT?JS_NULL:error(c);
  JSValue value=JS_NewObject(c);
  char boot[80];snprintf(boot,sizeof boot,"%lld:%lld",(long long)r.boot_sec,(long long)r.boot_usec);
  JS_SetPropertyStr(c,value,"boot",JS_NewString(c,boot));
  JS_SetPropertyStr(c,value,"request",JS_NewString(c,r.request));
  JS_SetPropertyStr(c,value,"sha256",JS_NewString(c,r.sha256));
  JS_SetPropertyStr(c,value,"stage",JS_NewInt32(c,r.stage));
  JS_SetPropertyStr(c,value,"pid",JS_NewInt32(c,r.pid));
  JS_SetPropertyStr(c,value,"plugin",JS_NewBool(c,r.plugin));
  JS_SetPropertyStr(c,value,"pluginId",JS_NewString(c,r.plugin_id));
  JS_SetPropertyStr(c,value,"pluginVersion",JS_NewString(c,r.plugin_version));
  JS_SetPropertyStr(c,value,"generation",JS_NewString(c,r.generation));
  JS_SetPropertyStr(c,value,"noSideEffects",JS_NewBool(c,r.no_side_effects));
  JS_SetPropertyStr(c,value,"message",JS_NewString(c,r.message));
  return value;
}
void pm_loader_bind(JSContext *context,JSValue api){
  JS_SetPropertyStr(context,api,"loaderStart",JS_NewCFunction(context,start,"loaderStart",3));
  JS_SetPropertyStr(context,api,"loaderPoll",JS_NewCFunction(context,poll,"loaderPoll",1));
  JS_SetPropertyStr(context,api,"pluginStart",JS_NewCFunction(context,plugin_start,"pluginStart",3));
}
