// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "game-source.h"
#include "native-operation.h"
#include <stdlib.h>
#include <string.h>
static JSValue failure(JSContext* c,const char* code,const char* message){
 JSValue error=JS_NewError(c);JS_SetPropertyStr(c,error,"code",JS_NewString(c,code));JS_SetPropertyStr(c,error,"message",JS_NewString(c,message));return JS_Throw(c,error);
}
static JSValue metadata(JSContext* c,JSValueConst self,int argc,JSValueConst* argv){
 (void)self;if(argc!=3)return JS_ThrowTypeError(c,"Expected PID, process generation and title");
 if(!pm_native_claim())return failure(c,"NATIVE_BUSY","A native operation is in progress");
 int32_t pid;int registered=0;const char* generation=JS_ToCString(c,argv[1]),*title=JS_ToCString(c,argv[2]);
 unsigned char* bytes=NULL;size_t length=0;char reason[192]="Invalid game metadata request";JSValue result=JS_UNDEFINED;
 if(generation&&title&&!JS_ToInt32(c,&pid,argv[0])&&!pm_game_metadata_read_ex(pid,generation,title,&bytes,&length,reason,sizeof reason,&registered)){
  result=JS_NewObject(c);JS_SetPropertyStr(c,result,"format",JS_NewString(c,!memcmp(title,"PPS",3)||!memcmp(title,"MOUU",4)?"json":"sfo"));
  JS_SetPropertyStr(c,result,"scope",JS_NewString(c,registered?"registered-running-title":"running-sandbox"));JS_SetPropertyStr(c,result,"bytes",JS_NewArrayBufferCopy(c,bytes,length));
  if(registered){JS_SetPropertyStr(c,result,"generation",JS_NewString(c,generation));JS_SetPropertyStr(c,result,"titleId",JS_NewString(c,title));}
 }
 free(bytes);if(generation)JS_FreeCString(c,generation);if(title)JS_FreeCString(c,title);pm_native_release();
 return JS_IsUndefined(result)?failure(c,"GAME_METADATA",reason):result;
}
void pm_game_bind(JSContext* c,JSValue api){JS_SetPropertyStr(c,api,"gameMetadata",JS_NewCFunction(c,metadata,"gameMetadata",3));}
