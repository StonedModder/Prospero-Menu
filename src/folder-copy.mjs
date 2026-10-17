// SPDX-License-Identifier: GPL-3.0-or-later
import path from 'node:path';
import {createHash} from 'node:crypto';
import {invariant} from './errors.mjs';

// Walk twice instead of persisting a potentially huge list in the job record.
// The final receipt contains one tree digest, including names and empty folders.
export function folderCopier({io,resolve,identity,same,syncFolder,copyFile,digest}) {
 const pause=()=>new Promise(resolve=>setTimeout(resolve,0));
 const cancelled=signal=>invariant(!signal?.aborted,'FILE_CANCELLED','Folder copy cancelled.');
 const add=(hash,value)=>hash.update(JSON.stringify(value)+'\n');
 async function* walk(root,signal) {
  const pending=[''];let device,count=0;
  while(pending.length){
   cancelled(signal);
   const relative=pending.pop(),source=relative?path.join(root,relative):root;
   await resolve(source);const st=await io.lstat(source);
   invariant(st.isDirectory()||st.isFile(),'FILE_TYPE','Folders containing special files cannot be copied.');
   device??=String(st.dev);
   invariant(String(st.dev)===device,'FILE_MOUNT','Copy mounted storage separately.');
   const entry={relative,source,directory:st.isDirectory(),...identity(st)};
   yield entry;
   if(entry.directory){
    const names=(await io.readdir(source)).sort();
    for(let i=names.length-1;i>=0;i--)pending.push(path.join(relative,names[i]));
   }
   if(++count%64===0)await pause();
  }
 }
 async function scan(root,signal,onProgress) {
  const hash=createHash('sha256');let bytes=0,files=0,folders=0,last=0;
  for await(const e of walk(root,signal)){
   add(hash,e);if(e.directory)folders++;else{files++;bytes+=e.bytes;}
   invariant(Number.isSafeInteger(bytes),'FILE_SIZE','The folder size exceeds the supported transfer size.');
   if(onProgress&&Date.now()-last>=250){await onProgress({completed:0,total:0,indeterminate:true,phase:`Scanning folder: ${files} files`});last=Date.now();}
  }
  return {signature:hash.digest('hex'),bytes,files,folders};
 }
 const treeEntry=(e,sha256)=>[e.relative,e.directory?'folder':'file',e.directory?0:e.bytes,...(e.directory?[]:[sha256])];
 async function treeDigest(root,signal){
  const hash=createHash('sha256');let bytes=0,files=0,folders=0;
  for await(const e of walk(root,signal)){
   let sha256;
   if(e.directory)folders++;
   else{
    const f=await io.open(e.source,'r');
    try{invariant(same(identity(await f.stat()),e),'FILE_CHANGED','A copied file changed.');sha256=await digest(f,signal);invariant(same(identity(await f.stat()),e),'FILE_CHANGED','A copied file changed.');}finally{await f.close();}
    bytes+=e.bytes;files++;
   }
   add(hash,treeEntry(e,sha256));
  }
  return {treeSha256:hash.digest('hex'),bytes,files,folders};
 }
 async function checkRoot(receipt){
  await resolve(receipt.output);const st=await io.lstat(receipt.output);
  invariant(st.isDirectory()&&String(st.dev)===receipt.identity.dev&&String(st.ino)===receipt.identity.ino,'FILE_CHANGED','The destination folder changed.');
 }
 return {
  async run(input,context){
   let created;
   try{
    const before=await scan(input.source,context.signal,context.progress);
    invariant(same(identity(await io.lstat(input.source)),input),'FILE_CHANGED','The selected folder changed. Select it again.');
    cancelled(context.signal);await resolve(input.destination);
    const capacity=await io.statfs(input.destination);
    invariant(Number(capacity.bsize)*Number(capacity.bavail)>=before.bytes+16*1048576,'NO_SPACE','The destination needs enough free space for the entire folder.');
    await io.mkdir(input.output);created=identity(await io.lstat(input.output));
    const receipt={directory:true,output:input.output,identity:created};
    const content=createHash('sha256'),sourceSnapshot=createHash('sha256');let completed=0,files=0;
    for await(const e of walk(input.source,context.signal)){
     add(sourceSnapshot,e);await checkRoot(receipt);
     const output=e.relative?path.join(input.output,e.relative):input.output;
     let sha256;
     if(e.directory){
      if(e.relative){await resolve(path.dirname(output));await io.mkdir(output);}
     }else{
      const child={...e,destination:path.dirname(output),output,destinationName:path.basename(output),name:path.basename(output),move:false};
      const result=await copyFile(child,{...context,checkpoint:async()=>{},progress:async p=>{
       await context.progress({completed:Math.min(before.bytes,completed+p.completed),total:before.bytes,phase:`${p.phase.startsWith('Verifying')?'Verifying':'Copying'} ${files+1}/${before.files}: ${e.relative}`.slice(0,191)});
      }});
      invariant(result.completed,'FILE_CANCELLED','Folder copy cancelled.');
      sha256=result.sha256;completed+=result.bytes;files++;
     }
     add(content,treeEntry(e,sha256));
    }
    invariant(sourceSnapshot.digest('hex')===before.signature,'FILE_CHANGED','The source folder changed while copying.');
    await context.progress({completed:before.bytes,total:before.bytes,phase:'Checking folder contents'});
    const after=await scan(input.source,context.signal);
    invariant(after.signature===before.signature,'FILE_CHANGED','The source folder changed while copying.');
    await checkRoot(receipt);
    // Flush empty directories too, before recording durable completion.
    for await(const e of walk(input.output,context.signal))if(e.directory)await syncFolder(e.source);
    await syncFolder(input.destination);cancelled(context.signal);
    Object.assign(receipt,{treeSha256:content.digest('hex'),bytes:before.bytes,files:before.files,folders:before.folders});
    await context.checkpoint(receipt);
    return {completed:true,...receipt,message:`Copied ${before.files} files and ${before.folders} folders to ${input.output}`};
   }catch(error){
    const suffix=created?` Partial copy kept at ${input.output}. Original folder unchanged.`:' Original folder unchanged.';
    if(error.code==='FILE_CANCELLED')return {cancelled:true,output:created?input.output:undefined,message:error.message+suffix};
    error.message+=suffix;throw error;
   }
  },
  async verify(result){await checkRoot(result);return true;},
  async reconcile(job){
   const c=job.checkpoint;if(!c?.directory||!c.treeSha256)return null;
   await checkRoot(c);const actual=await treeDigest(c.output);
   if(actual.treeSha256!==c.treeSha256||actual.bytes!==c.bytes||actual.files!==c.files||actual.folders!==c.folders)return null;
   return {completed:true,...c,message:'Verified folder copy saved at '+c.output+'. Original retained.'};
  }
 };
}
