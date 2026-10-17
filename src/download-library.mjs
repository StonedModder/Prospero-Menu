// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import { invariant } from './errors.mjs';
const finished=j=>['completed','failed','cancelled'].includes(j.status);
export function installDownloadLibrary(runtime){
 runtime.downloadLibrary={async remove({kind,id}){
  invariant(['payload','package'].includes(kind),'DOWNLOAD_KIND','Choose a payload or package.');
  const inbox=kind==='package'?runtime.packageInbox:runtime.inbox;
  invariant(inbox,'DOWNLOAD_UNAVAILABLE','Downloads are not available.');
  return inbox.exclusive(id,async()=>{
   // Reserve against new jobs while checking references and deleting owned files.
   await runtime.store.transaction(async state=>{
    const item=await inbox.status(id);
    invariant(!state.jobs.some(j=>!finished(j)&&(j.args?.id===id||j.args?.upload===id||j.result?.id===id||j.resources?.some(r=>r.endsWith(':'+id)))),
     'DOWNLOAD_BUSY','Finish or cancel the operation using this file first.');
    invariant(!state.startupPayloads?.some(x=>x.id===id),'DOWNLOAD_STARTUP','Remove this file from Startup payloads before deleting it.');
    invariant(!state.plugins?.some(x=>x.upload===id),'DOWNLOAD_PLUGIN','This file is registered as a plugin.');
    invariant(kind==='package'||!runtime.componentHashes?.has(item.expectedSha256)||state.jobs.some(j=>['payload.download','file.import'].includes(j.kind)&&j.result?.id===id),'DOWNLOAD_COMPONENT','This file is a bundled ProsperoMenu service.');
    for(const suffix of ['.payload','.part','.source','.json']){
     const file=inbox.file(id,suffix);try{const st=await fs.lstat(file);invariant(st.isFile()&&!st.isSymbolicLink(),'DOWNLOAD_FILE','The stored file changed.');await fs.unlink(file);}catch(e){if(e.code!=='ENOENT')throw e;}
    }
    await inbox.syncDirectory();
   });
   if(kind==='package')runtime.packageItems=await inbox.list();else runtime.inboxItems=await inbox.list();
   runtime.sequence++;return {deleted:true,message:'Downloaded copy deleted.'};
  });
 }};
}
