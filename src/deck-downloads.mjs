// SPDX-License-Identifier: GPL-3.0-or-later
import { payloadRunAction } from './deck-payload-action.mjs';
export function payloadDownloadsRows(state, view, managed) {
 const rows=[],row=(label,detail,command=null,args={},extra={})=>rows.push({label,detail,command,args,enabled:!!command,...extra});
 if(view.load){
  const load=view.load,job=state.jobs.find(j=>j.id===load.job);
  const phase=load.trackJob?(job?.status==='completed'?'ready':['failed','cancelled','interrupted','uncertain','held'].includes(job?.status)?'error':'loading'):load.phase;
  const message=load.trackJob?(job?.error?.message||job?.result?.message||job?.progress?.phase):load.error||load.message||job?.progress?.phase;
  row(phase==='ready'?'Load complete':phase==='error'?'Could not load payload':phase==='preparing'?'Preparing payload':'Loading payload',
    message||(phase==='ready'?'Payload sent to the loader.':phase==='preparing'?'Checking the selected ELF. Loading will follow automatically.':'Waiting for the payload loader.'));
  if(phase==='preparing'&&['queued','running','held'].includes(job?.status))row('Cancel','Stop before loading. Keep the original file.','job.cancel',{id:job.id});
  if(load.file&&['ready','error'].includes(phase))row('Payload options','Set startup or delete the stored copy.','$payload-file',{id:load.file});
  row('Back to payloads',['preparing','loading'].includes(phase)?'Loading continues in the background.':'','$payload-downloads-exit');return rows;
 }
 const userFile=id=>state.jobs.some(j=>['payload.download','file.import'].includes(j.kind)&&j.result?.id===id);
 const files=state.inbox.filter(x=>/\.elf$/i.test(x.name)&&(!managed?.has(x.expectedSha256)||userFile(x.id)));
 if(!view.id){
  for(const item of files)row(item.name,`${Math.ceil(item.bytes/1048576)} MiB · ${item.status==='verified'?'Ready':'Incomplete'}`,'$payload-file',{id:item.id});
  if(!files.length)row('No downloads yet','Download from a link or load a file from storage.');
 }else{
  const item=files.find(x=>x.id===view.id);
  if(item){
   if(item.status==='verified'){
    const action=payloadRunAction(state,{...item,sha256:item.expectedSha256},managed);
    row('Load payload',item.name+' · '+action.detail,action.command,action.args,{...action,confirm:false,payloadResult:true,payloadFile:item.id});
    const startup=state.startupPayloads?.items.find(x=>x.id===item.id);
    if(state.startupPayloads&&action.command!=='service.start'&&!managed?.has(item.expectedSha256)&&!state.jobs.some(j=>j.result?.id===item.id&&managed?.has(j.result.sha256)))row('Load on ProsperoMenu startup',startup?.enabled?'On':'Off',
     'payload.startup.configure',{id:item.id,enabled:!startup?.enabled,revision:state.revision},{on:!!startup?.enabled});
   }else row('Download incomplete','Delete this copy and download the file again.');
   row('Delete download','Delete this stored copy. Remove it from startup first if it is configured.',
    'download.delete',{kind:'payload',id:item.id},{confirm:true});
  }else row('File removed','This download is no longer stored.');
 }
 row(view.id?'Back to downloads':'Back to payloads','',view.id?'$payload-downloads':'$payload-downloads-exit');return rows;
}
