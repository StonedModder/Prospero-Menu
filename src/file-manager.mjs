// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {invariant} from './errors.mjs';
import {folderCopier} from './folder-copy.mjs';
const within=(p,r)=>p===r||p.startsWith(r+path.sep);
const identity=s=>({dev:String(s.dev),ino:String(s.ino),bytes:s.size,modified:s.mtimeMs});
const same=(a,b)=>a.dev===b.dev&&a.ino===b.ino&&a.bytes===b.bytes&&a.modified===b.modified;
const fileName=name=>{invariant(typeof name==='string'&&/^[^/\\\x00-\x1f\x7f]{1,180}$/.test(name)&&!['.','..'].includes(name)&&name.trim()===name,'FILE_NAME','Enter a name without slashes.');return name;};
export function installFileManager(runtime,{io=fs,roots=[{label:'Console storage (/data)',path:'/data'},{label:'Internal storage (/user)',path:'/user'},
 ...Array.from({length:8},(_,i)=>({label:'USB '+(i+1),path:'/mnt/usb'+i,usb:true}))],protectedPaths=['/data/ProsperoMenu']}={}){
 roots=roots.map(r=>({...r,path:path.resolve(r.path)}));protectedPaths=protectedPaths.map(p=>path.resolve(p));
 async function resolve(source){
  invariant(typeof source==='string'&&source.length<1024,'FILE_PATH','Choose a file or folder.');
  const target=path.resolve(source),root=roots.find(r=>within(target,r.path));
  invariant(root&&!protectedPaths.some(p=>within(target,p)),'FILE_PROTECTED','Choose a user file in console storage or USB. ProsperoMenu manages its own files through Downloads.');
  let p=root.path;
  for(const part of ['',...path.relative(p,target).split(path.sep).filter(Boolean)]){
   if(part)p=path.join(p,part);invariant(!(await io.lstat(p)).isSymbolicLink(),'FILE_LINK','Symbolic links cannot be selected.');
  }
  if(root.usb)invariant((await io.statfs(root.path)).mountPoint===root.path,'USB_DISCONNECTED','Connect the USB drive and refresh.');
  return {target,root};
 }
 async function browse({source=null}={}){
  if(!source){const entries=[];for(const r of roots){try{await resolve(r.path);entries.push({name:r.label,source:r.path,directory:true});}catch(e){if(!['ENOENT','USB_DISCONNECTED','FILE_LINK'].includes(e.code))throw e;}}
   return {source:null,parent:null,entries,usbMissing:!entries.some(e=>roots.some(r=>r.usb&&r.path===e.source))};}
  const {target,root}=await resolve(source);invariant((await io.lstat(target)).isDirectory(),'FILE_DIRECTORY','Choose a folder.');
  const names=await io.readdir(target,{withFileTypes:true});invariant(names.length<=4096,'FILE_DIRECTORY_LIMIT','Choose a smaller folder.');const entries=[];
  for(const item of names){const file=path.join(target,item.name);if(item.isSymbolicLink()||protectedPaths.some(p=>within(file,p)))continue;
   const st=await io.lstat(file);if(!st.isFile()&&!st.isDirectory())continue;
   entries.push({name:item.name,source:file,directory:st.isDirectory(),...identity(st)});
  }
  entries.sort((a,b)=>Number(b.directory)-Number(a.directory)||a.name.localeCompare(b.name));
  return {source:target,parent:target===root.path?null:path.dirname(target),entries};
 }
 async function validate(input){
  const {target:source,root}=await resolve(input.source),{target:destination}=await resolve(input.destination);
  const st=await io.lstat(source);invariant(source!==root.path&&(st.isFile()||st.isDirectory())&&same(identity(st),input),'FILE_CHANGED','The selected file or folder changed. Select it again.');
  invariant(typeof input.move==='boolean','FILE_ACTION','Choose Copy or Move.');
  invariant(!st.isDirectory()||!input.move,'FILE_FOLDER_MOVE','Use Copy for folders. Folder moves are not supported.');
  invariant((await io.lstat(destination)).isDirectory(),'FILE_DIRECTORY','Choose a destination folder.');
  const destinationName=input.destinationName==null?path.basename(source):fileName(input.destinationName);
  const output=path.join(destination,destinationName);
  invariant(!protectedPaths.some(p=>within(output,p)),'FILE_PROTECTED','ProsperoMenu manages its own storage.');
  invariant(output!==source,'FILE_SAME_FOLDER','Choose another folder.');
  invariant(!st.isDirectory()||!within(output,source),'FILE_RECURSIVE','Choose a destination outside the folder being copied.');
  try{await io.lstat(output);invariant(false,'FILE_EXISTS','A file or folder with that name already exists. Choose another folder.');}catch(e){if(e.code!=='ENOENT')throw e;}
  const capacity=await io.statfs(destination);invariant(Number(capacity.bsize)*Number(capacity.bavail)>=(st.isFile()?st.size:0)+16*1048576,'NO_SPACE','The destination needs enough free space for this copy.');
  return {source,destination,output,destinationName,move:input.move,name:path.basename(source),directory:st.isDirectory(),...identity(st)};
 }
 const pause=()=>new Promise(r=>setTimeout(r,0));
 async function digest(file,signal){const hash=createHash('sha256'),buffer=Buffer.alloc(65536);let at=0,last=Date.now();for(;;){invariant(!signal?.aborted,'FILE_CANCELLED','Transfer cancelled.');const {bytesRead}=await file.read(buffer,0,buffer.length,at);if(!bytesRead)break;hash.update(buffer.subarray(0,bytesRead));at+=bytesRead;if(Date.now()-last>20){await pause();last=Date.now();}}return hash.digest('hex');}
 async function syncFolder(folder){if(process.platform==='win32')return;const f=await io.open(folder,'r');try{await f.sync();}finally{await f.close();}}
 runtime.fileManager={browse};
 const resources=()=>['local-file-copy'];
 async function absent(file){try{await io.lstat(file);return false;}catch(e){if(e.code!=='ENOENT')throw e;return true;}}
 async function deleteTarget(input){const {target,root}=await resolve(input.source),st=await io.lstat(target);
  invariant(target!==root.path&&(st.isFile()||st.isDirectory())&&same(identity(st),input),'FILE_CHANGED','The selected file changed. Select it again.');
  if(st.isDirectory())invariant((await io.readdir(target)).length===0,'FILE_NOT_EMPTY','Only empty folders can be deleted.');
  return {source:target,directory:st.isDirectory(),name:path.basename(target),...identity(st)};
 }
 runtime.jobs.register('file.delete',{validate:deleteTarget,resources,cancellable:false,
  async run(input,context){await deleteTarget(input);await context.checkpoint({source:input.source,checked:true});
   if(input.directory)await io.rmdir(input.source);else await io.unlink(input.source);await syncFolder(path.dirname(input.source));
   return {completed:true,source:input.source,message:'Deleted '+input.name};},
  verify:result=>absent(result.source),
  async reconcile(job){if(!job.checkpoint?.checked)return null;await resolve(path.dirname(job.args.source));return await absent(job.args.source)?{completed:true,source:job.args.source,message:'File is no longer present.'}:null;}
 });
 async function directoryTarget(input){const {target:destination}=await resolve(input.destination);invariant((await io.lstat(destination)).isDirectory(),'FILE_DIRECTORY','Choose a folder.');
  const name=fileName(input.name),output=path.join(destination,name);invariant(!protectedPaths.some(p=>within(output,p)),'FILE_PROTECTED','ProsperoMenu manages its own storage.');
  invariant(await absent(output),'FILE_EXISTS','That name already exists. Choose another name.');return {destination,name,output};
 }
 runtime.jobs.register('file.mkdir',{validate:directoryTarget,resources,cancellable:false,
  async run(input,context){await directoryTarget(input);await io.mkdir(input.output);const st=identity(await io.lstat(input.output));await context.checkpoint({output:input.output,identity:st});await syncFolder(input.destination);return {completed:true,output:input.output,identity:st,message:'Created '+input.output};},
  async verify(result){await resolve(result.output);const st=await io.lstat(result.output);return st.isDirectory()&&String(st.dev)===result.identity.dev&&String(st.ino)===result.identity.ino;},
  async reconcile(job){return job.checkpoint?.identity?{completed:true,...job.checkpoint,message:'Folder created.'}:null;}
 });
 async function copyFile(input,context){
   let source,output,created,verified=false;
   try{
    await validate(input);source=await io.open(input.source,'r');invariant(same(identity(await source.stat()),input),'FILE_CHANGED','The source file changed.');
    output=await io.open(input.output,'wx');created=identity(await output.stat());
    const hash=createHash('sha256'),buffer=Buffer.alloc(65536);let at=0,last=0;
    while(at<input.bytes){
     invariant(!context.signal.aborted,'FILE_CANCELLED','Transfer cancelled.');await resolve(input.destination);
     const {bytesRead}=await source.read(buffer,0,Math.min(buffer.length,input.bytes-at),at);invariant(bytesRead>0,'FILE_READ','The source ended unexpectedly.');
     hash.update(buffer.subarray(0,bytesRead));for(let offset=0;offset<bytesRead;){const {bytesWritten}=await output.write(buffer,offset,bytesRead-offset,at+offset);invariant(bytesWritten>0,'FILE_WRITE','The destination stopped accepting data.');offset+=bytesWritten;}
     at+=bytesRead;if(Date.now()-last>=250){await context.progress({completed:at,total:input.bytes,phase:((input.move?'Moving ':'Copying ')+input.name).slice(0,191)});last=Date.now();await pause();}
    }
    await output.sync();await output.close();output=null;await syncFolder(input.destination);
    await context.progress({completed:input.bytes,total:input.bytes,phase:'Verifying copied file'});
    const check=await io.open(input.output,'r');let sha256;try{const st=await check.stat();invariant(String(st.ino)===created.ino&&String(st.dev)===created.dev,'FILE_CHANGED','Destination identity changed.');sha256=await digest(check,context.signal);}finally{await check.close();}
    invariant(sha256===hash.digest('hex'),'FILE_VERIFY','The copy did not match. Original retained.');
    invariant(same(identity(await source.stat()),input),'FILE_CHANGED','The source changed during copying. Original retained.');
    verified=true;await context.checkpoint({output:input.output,sha256,bytes:input.bytes});
    if(input.move){invariant(!context.signal.aborted,'FILE_CANCELLED','Copy finished; move cancelled. Original retained.');await resolve(input.output);const copied=await io.lstat(input.output);invariant(String(copied.dev)===created.dev&&String(copied.ino)===created.ino&&copied.size===input.bytes,'FILE_CHANGED','Destination changed; original retained.');await resolve(input.source);invariant(same(identity(await io.lstat(input.source)),input),'FILE_CHANGED','Original changed; verified copy retained.');await io.unlink(input.source);await syncFolder(path.dirname(input.source));}
    return {completed:true,output:input.output,sha256,bytes:input.bytes,message:(input.move?'Moved to ':'Copied to ')+input.output};
   }catch(error){
    if(created&&!verified){try{const st=await io.lstat(input.output);if(String(st.dev)===created.dev&&String(st.ino)===created.ino)await io.unlink(input.output);}catch{/* Preserve a changed or disconnected destination. */}}
    if(error.code==='FILE_CANCELLED')return {cancelled:true,message:error.message};throw error;
   }finally{await source?.close();await output?.close();}
 }
 const folders=folderCopier({io,resolve,identity,same,syncFolder,copyFile,digest});
 runtime.jobs.register('file.transfer',{
  validate,resources,timeout:48*60*60*1000,cancellable:true,
  async run(input,context){const checked=await validate(input);return checked.directory?folders.run(checked,context):copyFile(checked,context);},
  async verify(result){if(result.directory)return folders.verify(result);const {target}=await resolve(result.output);return (await io.lstat(target)).size===result.bytes;},
  async reconcile(job){
   if(job.checkpoint?.directory)return folders.reconcile(job);
   const c=job.checkpoint;if(!c?.sha256)return null;await resolve(c.output);const f=await io.open(c.output,'r');try{if(await digest(f)!==c.sha256)return null;}finally{await f.close();}
   let original=true;try{await io.lstat(job.args.source);}catch(e){if(e.code!=='ENOENT')throw e;original=false;}
   return {completed:true,...c,message:original?'Verified copy saved at '+c.output+'. Original retained.':'Moved to '+c.output};
  }
 });
}
