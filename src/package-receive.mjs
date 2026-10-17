// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant, PmError } from './errors.mjs';
import { downloadUrl } from './download-url.mjs';
import { MAX_PACKAGE } from './package-format.mjs';
const sleep = ms => new Promise(resolve=>setTimeout(resolve,ms));
export function installPackageReceiving(runtime,{delay=sleep}={}) {
  runtime.jobs.register('package.download',{
    validate(input){
      invariant(runtime.downloads,'PACKAGE_DOWNLOAD','Package downloading is unavailable.');
      invariant(typeof input.name==='string' && /^[^/\\\x00-\x1f]{1,180}\.pkg$/i.test(input.name),'PACKAGE_NAME','Choose a name ending in .pkg.');
      return {url:downloadUrl(input.url),name:input.name};
    },
    resources:()=>['native-download'],timeout:49*60*60*1000,cancellable:true,
    async run(input,context){let file;
      try{
        file=await runtime.downloads.fetchToFile(input.url,MAX_PACKAGE,context,{timeoutSeconds:48*60*60});
        const proof=await runtime.packageInbox.adoptDownload(file.source,input.name,context);
        return {completed:true,id:proof.id,sha256:proof.sha256,package:proof,message:'Package downloaded and verified.'};
      }catch(error){
        if(context.signal.aborted)return {cancelled:true,message:'Package download stopped; installation was not submitted.'};
        error.noSideEffects=true;throw error;
      }finally{file?.release();runtime.packageItems=await runtime.packageInbox.list();runtime.sequence++;}
    },
    async verify(result){return (await runtime.packageInbox.status(result.id)).status==='verified';},
    async reconcile(){return {absent:true};}
  });
  async function wait(id,context,phase){
    for(;;){
      invariant(!context.signal.aborted,'PACKAGE_RECEIVE_INTERRUPTED','Package workflow stopped. Check its recorded child operation before retrying.');
      const child=await runtime.jobs.get(id);invariant(child,'PACKAGE_RECEIVE_CHILD','The package operation receipt is missing.');
      if(child.status==='completed')return child;
      if(['failed','cancelled','interrupted','uncertain','held'].includes(child.status)){
        const error=new PmError(child.error?.code||'PACKAGE_RECEIVE_STOPPED',child.error?.message||`Package operation ${child.status}.`,{action:'Open Transfers to inspect the existing operation. No second install was submitted.'});
        if(['failed','cancelled'].includes(child.status))error.noSideEffects=true;throw error;
      }
      await context.progress({completed:0,total:1,...child.progress,phase:phase+(child.progress?.phase?' · '+child.progress.phase:'')});await delay(250);
    }
  }
  runtime.jobs.register('package.receive',{
    async validate(input){
      invariant(runtime.capabilities.get('packages')?.available,'PACKAGE_UNAVAILABLE','Package installation is unavailable.');
      const contentId=input.contentId||'';invariant(typeof contentId==='string' && contentId.length<=80 && !/[\x00-\x1f]/.test(contentId),'PACKAGE_ID','Invalid expected content ID.');
      if(input.source){
        invariant(runtime.localFiles,'PACKAGE_STORAGE','Storage browsing is unavailable.');
        return {...await runtime.localFiles.validate({...input,kind:'package'}),contentId};
      }
      if(input.id){const item=await runtime.packageInbox.status(input.id);invariant(item.offset===item.bytes,'PACKAGE_INCOMPLETE','Finish the package upload first.');return {id:item.id,name:item.name,contentId};}
      const url=downloadUrl(input.url);const name=input.name||'received.pkg';invariant(/^[^/\\\x00-\x1f]{1,180}\.pkg$/i.test(name),'PACKAGE_NAME','Choose a name ending in .pkg.');return {url,name,contentId};
    },
    // One parent holds a queue slot; the second remains available to its child.
    resources:()=>['dpi-pipeline'],timeout:99*60*60*1000,cancellable:false,
    async run(input,context){
      const kind=input.source?'file.import':input.id?'package.verify':'package.download';
      const child=await runtime.jobs.enqueue(kind,input.source?{...input,kind:'package'}:input.id?{id:input.id}:{url:input.url,name:input.name},'dpi_stage_'+context.id);
      await context.checkpoint({stage:child.id});
      const staged=await wait(child.id,context,'Preparing package'),proof=staged.result;
      if(input.contentId && input.contentId!==proof.package.contentId)throw Object.assign(new PmError('PACKAGE_CONTENT_ID','The downloaded package does not match the supplied content ID.'),{noSideEffects:true});
      const install=await runtime.jobs.enqueue('package.install',{id:proof.id},'dpi_install_'+context.id);
      await context.checkpoint({stage:child.id,install:install.id});
      const done=await wait(install.id,context,'Installing package');
      return {completed:true,install:done.id,contentId:done.result.contentId,message:'Package installed; the console confirmed completion.'};
    },
    async verify(result){const child=await runtime.jobs.get(result.install);return child?.kind==='package.install' && child.status==='completed' && child.result?.completed===true;},
    async reconcile(job){
      // Never enqueue a new installation when reconciling a disconnected one.
      const child=job.checkpoint?.install ? await runtime.jobs.get(job.checkpoint.install) :
        runtime.jobs.snapshot().find(x=>x.key==='dpi_install_'+job.id) || await runtime.jobs.archive.find(runtime.store.snapshot(),'dpi_install_'+job.id);
      if(child?.status==='completed')return {completed:true,install:child.id,contentId:child.result.contentId,message:'Recorded package installation completed.'};
      if(child?.status==='failed' && child.result?.priorOutcome==='unknown' && child.error?.code==='PREVIOUS_BOOT_ENDED')return {expired:true};
      if(!child || ['failed','cancelled'].includes(child.status))return {absent:true};
      return null;
    },
    async verifyExpired(_result,job){
      const child=job.checkpoint?.install?await runtime.jobs.get(job.checkpoint.install):runtime.jobs.snapshot().find(x=>x.key==='dpi_install_'+job.id);
      return child?.status==='failed' && child.result?.priorOutcome==='unknown' && child.error?.code==='PREVIOUS_BOOT_ENDED';
    }
  });
}
