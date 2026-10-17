// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "sha256.h"
#include <stdlib.h>
#include <stdint.h>
static JSClassID hash_class;
typedef struct {SHA256_CTX state;int finalized;} Hash;
static void finalize(JSRuntime* rt,JSValue value){(void)rt;free(JS_GetOpaque(value,hash_class));}
static JSValue create(JSContext* c,JSValueConst self,int argc,JSValueConst* args){
 (void)self;(void)args;if(argc)return JS_ThrowTypeError(c,"Expected no hash arguments");
 Hash*h=calloc(1,sizeof *h);if(!h)return JS_ThrowOutOfMemory(c);sha256_init(&h->state);
 JSValue value=JS_NewObjectClass(c,hash_class);if(JS_IsException(value)){free(h);return value;}JS_SetOpaque(value,h);return value;
}
static JSValue update(JSContext*c,JSValueConst self,int argc,JSValueConst*args){
 (void)self;if(argc!=4)return JS_ThrowTypeError(c,"Expected hash, buffer, offset and length");
 Hash*h=JS_GetOpaque2(c,args[0],hash_class);if(!h)return JS_EXCEPTION;
 if(h->finalized)return JS_ThrowTypeError(c,"Hash is finalized");
 size_t bytes=0;uint8_t*data=JS_GetArrayBuffer(c,&bytes,args[1]);if(!data)return JS_EXCEPTION;
 int64_t offset,length;if(JS_ToInt64(c,&offset,args[2])||JS_ToInt64(c,&length,args[3]))return JS_EXCEPTION;
 if(offset<0||length<0||length>1048576||(uint64_t)offset>bytes||(uint64_t)length>bytes-(uint64_t)offset)return JS_ThrowRangeError(c,"Invalid hash buffer range");
 sha256_update(&h->state,data+offset,(size_t)length);return JS_UNDEFINED;
}
static JSValue digest(JSContext*c,JSValueConst self,int argc,JSValueConst*args){
 (void)self;if(argc!=1)return JS_ThrowTypeError(c,"Expected hash");Hash*h=JS_GetOpaque2(c,args[0],hash_class);if(!h)return JS_EXCEPTION;
 if(h->finalized)return JS_ThrowTypeError(c,"Hash is finalized");uint8_t out[32];sha256_final(&h->state,out);h->finalized=1;
 return JS_NewArrayBufferCopy(c,out,sizeof out);
}
int pm_hash_bind(JSContext*c,JSValue api){
 if(!hash_class)JS_NewClassID(&hash_class);JSClassDef def={.class_name="ProsperoMenuSHA256",.finalizer=finalize};
 if(JS_NewClass(JS_GetRuntime(c),hash_class,&def))return -1;
 JS_SetPropertyStr(c,api,"hashCreate",JS_NewCFunction(c,create,"hashCreate",0));
 JS_SetPropertyStr(c,api,"hashUpdate",JS_NewCFunction(c,update,"hashUpdate",4));
 JS_SetPropertyStr(c,api,"hashDigest",JS_NewCFunction(c,digest,"hashDigest",1));return 0;
}
