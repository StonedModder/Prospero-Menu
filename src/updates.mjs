// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {PayloadInbox} from './inbox.mjs';
import {parseUpdateHeader,UPDATE_HEADER,UPDATE_MAX} from './update-format.mjs';
import {inspectPayload} from './payload-format.mjs';
import {invariant,clone,publicError} from './errors.mjs';
async function inspect(file,prefix,status,firmware){
  const header=parseUpdateHeader(prefix,{totalBytes:status.bytes,firmware});
  const elf=Buffer.alloc(Math.min(16384,header.bytes));let offset=0;
  while(offset<elf.length){const {bytesRead}=await file.read(elf,offset,elf.length-offset,UPDATE_HEADER+offset);invariant(bytesRead>0,'UPDATE_TRUNCATED','The update executable ended early.');offset+=bytesRead;}
  inspectPayload(elf,'prosperomenu-runtime.elf',{totalBytes:header.bytes});return header;
}
const matches=(image,args)=>image?.sha256===args.payloadHash&&image?.source===args.source;
export async function installUpdates(runtime,native,root='/data/ProsperoMenu/updates-compatibility'){
  await fs.mkdir(root,{recursive:true});invariant(!(await fs.lstat(root)).isSymbolicLink(),'UPDATE_PATH','The update directory cannot be a link.');
  const inbox=await new PayloadInbox(path.join(root,'incoming'),{id:runtime.id,reservationGroup:runtime.inbox?.reservations,policy:{minimum:UPDATE_HEADER+64,maximum:UPDATE_HEADER+UPDATE_MAX,
    names:/^[^/\\\x00-\x1f]{1,180}\.prosperomenu$/i,nameMessage:'Choose a .prosperomenu update package.',sizeMessage:'ProsperoMenu update packages are limited to 64 MiB plus their header.',
    inspect:(file,prefix,status)=>inspect(file,prefix,status,runtime.identity.firmware)}}).init();
  let state=null,fault=null,items=await inbox.list(),retained=[],retainedTotal=0,inventoryError=null;
  const refresh=()=>{try{state=native.updateStatus();fault=null;}catch(error){fault=publicError(error);}runtime.sequence++;return snapshot();};
  const protectedBy=hash=>[['good','Verified startup'],['previous','Previous build'],['candidate','Next selection'],['attempt','Recorded launch']].filter(([key])=>state?.[key]?.sha256===hash).map(([,label])=>label);
  const snapshot=()=>({available:!fault,state:clone(state),error:clone(fault),items:clone(items),retained:retained.map(entry=>{
    const reasons=protectedBy(entry.sha256);return {...entry,retainedBy:reasons,removable:entry.status==='inspected'&&!reasons.length&&!fault&&!state?.recovered};
  }),retainedTotal,inventoryError:clone(inventoryError)});
  async function inventory(){
    try{
      const names=(await fs.readdir(root)).filter(name=>/^[a-f0-9]{64}\.prosperomenu$/.test(name)).sort(),next=[];
      for(const name of names.slice(0,128)){
        const sha256=name.slice(0,64),file=path.join(root,name);let input;
        try{
          const st=await fs.lstat(file);invariant(st.isFile()&&!st.isSymbolicLink(),'UPDATE_FILE','Not a regular package file.');
          input=await fs.open(file,'r');const prefix=Buffer.alloc(UPDATE_HEADER);let offset=0;
          while(offset<prefix.length){const {bytesRead}=await input.read(prefix,offset,prefix.length-offset,offset);invariant(bytesRead>0,'UPDATE_TRUNCATED','The update header ended early.');offset+=bytesRead;}
          const h=parseUpdateHeader(prefix,{totalBytes:st.size});invariant(h.sha256===sha256,'UPDATE_CHANGED','Package name and header differ.');
          next.push({sha256,label:h.label,source:h.source,bytes:st.size,status:'inspected'});
        }catch{next.push({sha256,label:'Unreadable package',bytes:null,status:'unreadable'});}
        finally{if(input)await input.close();}
      }
      retained=next;retainedTotal=names.length;inventoryError=null;
    }catch(error){inventoryError=publicError(error);}runtime.sequence++;
  }
  const syncDir=async()=>{if(process.platform==='win32')return;const dir=await fs.open(root,'r');try{await dir.sync();}finally{await dir.close();}};
  async function verifyFile(file,args){
    const stat=await fs.lstat(file);invariant(stat.isFile()&&!stat.isSymbolicLink()&&stat.size===args.bytes+UPDATE_HEADER,'UPDATE_FILE','The staged update has changed.');
    const input=await fs.open(file,'r'),buffer=Buffer.alloc(65536),hash=createHash('sha256');let header;
    try{const prefix=Buffer.alloc(UPDATE_HEADER);let offset=0;
      while(offset<prefix.length){const {bytesRead}=await input.read(prefix,offset,prefix.length-offset,offset);invariant(bytesRead>0,'UPDATE_TRUNCATED','Update header ended early.');offset+=bytesRead;}
      header=parseUpdateHeader(prefix,{totalBytes:stat.size,firmware:runtime.identity.firmware});
      invariant(matches({...header},args),'UPDATE_CHANGED','Update identity differs from the accepted request.');
      let yieldAt=Date.now();for(let position=UPDATE_HEADER;position<stat.size;){const {bytesRead}=await input.read(buffer,0,Math.min(buffer.length,stat.size-position),position);invariant(bytesRead>0,'UPDATE_TRUNCATED','Update file ended early.');hash.update(buffer.subarray(0,bytesRead));position+=bytesRead;if(Date.now()-yieldAt>=25){await new Promise(resolve=>setTimeout(resolve,0));yieldAt=Date.now();}}
      invariant(hash.digest('hex')===args.payloadHash,'UPDATE_CHECKSUM','The staged executable checksum differs.');
    }finally{await input.close();}return header;
  }
  runtime.jobs.register('update.stage',{
    resources:()=>['update-selection','file-verification'],timeout:10*60*1000,cancellable:false,
    async validate(input){
      refresh();invariant(!fault&&!state.recovered,'UPDATE_STATE',fault?.message||'Update selection needs repair.');
      invariant(state.phase==='idle'&&input.revision===state.revision,'UPDATE_CHANGED','Refresh Updates before selecting another build.');
      const file=await inbox.status(input.id);invariant(file.offset===file.bytes,'UPLOAD_INCOMPLETE','Finish uploading the update package first.');
      const handle=await fs.open(inbox.file(file.id,file.status==='verified'?'.payload':'.part'),'r');let info;
      try{const prefix=Buffer.alloc(UPDATE_HEADER);const read=await handle.read(prefix,0,prefix.length,0);invariant(read.bytesRead===prefix.length,'UPDATE_TRUNCATED','Update header ended early.');info=await inspect(handle,prefix,file,runtime.identity.firmware);}finally{await handle.close();}
      invariant(info.sha256!==state.good?.sha256,'UPDATE_CURRENT','This executable is already the confirmed build.');
      return {id:file.id,revision:input.revision,payloadHash:info.sha256,source:info.source,bytes:info.bytes,label:info.label};
    },
    async run(args,context){
      let selectionAttempted=false;
      try{
        const verified=await inbox.verify(args.id,context);
        invariant(verified.source===args.source&&verified.bytes===args.bytes,'UPDATE_CHANGED','Uploaded update metadata changed.');
        const final=path.join(root,args.payloadHash+'.prosperomenu');let exists=false;
        try{await fs.lstat(final);exists=true;}catch(error){if(error.code!=='ENOENT')throw error;}
        if(!exists){
          const space=await fs.statfs(root);invariant(Number(space.bsize)*Number(space.bavail)>=args.bytes+UPDATE_HEADER+16*1024*1024,'NO_SPACE','Keep space for the staged build and a 16 MiB reserve. Existing builds are retained.');
          const part=path.join(root,context.id+'.part'),source=await fs.open(inbox.file(args.id,'.payload'),'r');let dest;
          try{dest=await fs.open(part,'wx',0o600);const chunk=Buffer.alloc(65536);let position=0,yieldAt=Date.now(),progressAt=0;
            while(position<args.bytes+UPDATE_HEADER){const {bytesRead}=await source.read(chunk,0,Math.min(chunk.length,args.bytes+UPDATE_HEADER-position),position);invariant(bytesRead>0,'UPDATE_TRUNCATED','The upload changed during staging.');await dest.writeFile(chunk.subarray(0,bytesRead));position+=bytesRead;
              if(Date.now()-progressAt>=250||position===args.bytes+UPDATE_HEADER){await context.progress({phase:'Staging ProsperoMenu update',completed:position,total:args.bytes+UPDATE_HEADER});progressAt=Date.now();}if(Date.now()-yieldAt>=25){await new Promise(resolve=>setTimeout(resolve,0));yieldAt=Date.now();}}
            await dest.sync();
          }finally{await source.close();if(dest)await dest.close();}
          await verifyFile(part,args);await fs.rename(part,final);await syncDir();
        }
        await verifyFile(final,args);selectionAttempted=true;state=native.updateStage(args.payloadHash,args.revision);fault=null;runtime.sequence++;
        return {completed:true,payloadHash:args.payloadHash,source:args.source,message:'Update staged and verified. The running runtime has not changed. Review and activate it for the next boot.'};
      }catch(error){if(!selectionAttempted)error.noSideEffects=true;throw error;}
      finally{items=await inbox.list();await inventory();runtime.sequence++;}
    },
    verify(_result,args){refresh();return !fault&&(matches(state?.candidate,args)||matches(state?.good,args));},
    reconcile(job){refresh();if(fault)return null;if(matches(state?.candidate,job.args)||matches(state?.good,job.args))return {completed:true,reconciled:true};
      return state.revision===job.args.revision&&state.phase==='idle'?{absent:true}:null;}
  });
  const idleSelection=()=>invariant(!runtime.jobs.snapshot().some(j=>j.kind==='update.stage'&&!['completed','failed','cancelled'].includes(j.status)),'UPDATE_BUSY','Finish or reconcile the current update staging operation first.');
  const service={inbox,snapshot,async refresh(){refresh();await inventory();return snapshot();},
    async begin(args){const result=await inbox.begin(args);items=await inbox.list();return result;},status:args=>inbox.status(args.id),
    async list(){items=await inbox.list();return items;},
    select(action,args){
      idleSelection();
      invariant(['arm','revert','discard'].includes(action),'UPDATE_ACTION','Unknown update action.');
      if(action==='arm')invariant(state?.good,'UPDATE_BASELINE','Run prosperomenu.elf and finish its startup once before activating an update. This establishes a verified fallback build.');
      state=action==='arm'?native.updateArm(args.revision):action==='revert'?native.updateRevert(args.revision):native.updateDiscard(args.revision);fault=null;runtime.sequence++;return snapshot();
    },
    async retire(args){
      idleSelection();refresh();invariant(!fault&&!state.recovered,'UPDATE_STATE',fault?.message||'Update selection needs repair.');
      invariant(args.revision===state.revision,'UPDATE_CHANGED','Refresh retained builds before removing a package.');
      invariant(typeof args.sha256==='string'&&/^[a-f0-9]{64}$/.test(args.sha256),'UPDATE_FILE','Choose a retained package.');
      invariant(!protectedBy(args.sha256).length,'UPDATE_RETAINED','Startup still retains this build.');
      invariant(retained.some(entry=>entry.sha256===args.sha256&&entry.status==='inspected'),'UPDATE_FILE','Refresh and inspect this retained package before removing it.');
      try{state=native.updateRetire(args.sha256,args.revision);fault=null;}
      finally{refresh();await inventory();}
      return snapshot();
    },
    confirmStartup(){try{state=native.updateAcknowledge();fault=null;}catch(error){fault=publicError(error);}runtime.sequence++;return snapshot();}
  };
  runtime.updates=service;refresh();await inventory();runtime.capability('updates',true,'Verified packages and deferred next-boot selection through the ProsperoMenu launcher.');return service;
}
