// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickjs.h"
#include "sqlite3.h"
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
static int deadline(void* p){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);struct timespec* end=p;return now.tv_sec>end->tv_sec||(now.tv_sec==end->tv_sec&&now.tv_nsec>=end->tv_nsec);}
static JSValue catalog(JSContext*c,JSValueConst self,int argc,JSValueConst*argv){
 (void)self;(void)argc;(void)argv;sqlite3*db=0;sqlite3_stmt*stmt=0;JSValue result=JS_UNDEFINED;int rc;const char*stage="open";
 const char*path="/system_data/priv/mms/app.db";struct stat st;
 int statrc=lstat(path,&st);
 if(statrc||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)){fprintf(stderr,"ProsperoMenu library catalog: database unavailable stat=%d errno=%d\n",statrc,statrc?errno:0);return JS_ThrowInternalError(c,"Installed-game database is unavailable");}
 rc=sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,0);
 if(rc!=SQLITE_OK)goto error;
 sqlite3_busy_timeout(db,50);sqlite3_limit(db,SQLITE_LIMIT_LENGTH,1048576);
 struct timespec end;clock_gettime(CLOCK_MONOTONIC,&end);end.tv_nsec+=150000000;if(end.tv_nsec>=1000000000){end.tv_sec++;end.tv_nsec-=1000000000;}sqlite3_progress_handler(db,1000,deadline,&end);
 stage="prepare";
 rc=sqlite3_prepare_v2(db,"SELECT titleId,titleName,contentId,size,AppInfoJson FROM tbl_contentinfo WHERE size>0 AND appType=1 LIMIT 8193",-1,&stmt,0);
 // Metadata is optional on older database schemas; the library must still work.
 if(rc!=SQLITE_OK&&strstr(sqlite3_errmsg(db),"no such column: AppInfoJson")){
  sqlite3_finalize(stmt);stmt=NULL;
  rc=sqlite3_prepare_v2(db,"SELECT titleId,titleName,contentId,size,NULL FROM tbl_contentinfo WHERE size>0 AND appType=1 LIMIT 8193",-1,&stmt,0);
 }
 if(rc!=SQLITE_OK)goto error;
 stage="read";result=JS_NewArray(c);unsigned row=0,skipped=0;
 while((rc=sqlite3_step(stmt))==SQLITE_ROW){
  if(row>=8192)goto error;const char*title=(const char*)sqlite3_column_text(stmt,0),*name=(const char*)sqlite3_column_text(stmt,1),*content=(const char*)sqlite3_column_text(stmt,2);
  if(!title||strlen(title)!=9){skipped++;continue;}
  // Display metadata is optional; JS normalizes it per row. Null metadata
  // must not hide an otherwise installed title.
  if(!name)name=title;if(!content||strlen(content)>128)content="";
  JSValue item=JS_NewObject(c);JS_SetPropertyStr(c,item,"titleId",JS_NewString(c,title));JS_SetPropertyStr(c,item,"name",JS_NewString(c,name));JS_SetPropertyStr(c,item,"contentId",JS_NewString(c,content));JS_SetPropertyStr(c,item,"bytes",JS_NewFloat64(c,sqlite3_column_double(stmt,3)));
  // Read installed metadata, never the potentially newer online catalog version.
  const char*json=(const char*)sqlite3_column_text(stmt,4);int jsonBytes=sqlite3_column_bytes(stmt,4);
  if(json&&jsonBytes>0&&jsonBytes<=1048576){
   JSValue info=JS_ParseJSON(c,json,(size_t)jsonBytes,"installed-game-metadata");
   if(JS_IsException(info)){JSValue error=JS_GetException(c);JS_FreeValue(c,error);}
   else {JSValue ver=JS_GetPropertyStr(c,info,!strncmp(title,"PPS",3)||!strncmp(title,"MOUU",4)?"CONTENT_VERSION":"APP_VER");
    if(JS_IsException(ver)){JSValue error=JS_GetException(c);JS_FreeValue(c,error);}else if(JS_IsString(ver))JS_SetPropertyStr(c,item,"version",JS_DupValue(c,ver));
    JS_FreeValue(c,ver);}
   JS_FreeValue(c,info);
  }
  JS_SetPropertyUint32(c,result,row++,item);
 }
 if(rc!=SQLITE_DONE)goto error;fprintf(stderr,"ProsperoMenu library catalog: rows=%u skipped=%u\n",row,skipped);sqlite3_finalize(stmt);sqlite3_close(db);return result;
error:
 fprintf(stderr,"ProsperoMenu library catalog: stage=%s rc=%d extended=%d error=%.256s\n",stage,rc,db?sqlite3_extended_errcode(db):rc,db?sqlite3_errmsg(db):"database open failed");
 JS_FreeValue(c,result);sqlite3_finalize(stmt);if(db)sqlite3_close(db);return JS_ThrowInternalError(c,"Could not read installed games. Try Refresh library again.");
}
void pm_catalog_bind(JSContext*c,JSValue api){JS_SetPropertyStr(c,api,"installedGames",JS_NewCFunction(c,catalog,"installedGames",0));}
