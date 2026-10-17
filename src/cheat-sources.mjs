// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import { sha1 } from '@noble/hashes/sha1';
import { invariant } from './errors.mjs';
const sources=['etaHEN/PS5_Cheats','GoldHEN/GoldHEN_Cheat_Repository'];
const hash=/^[a-f0-9]{40}$/;
export function selectCheatFiles(tree,titles){
  invariant(tree&&tree.truncated===false&&Array.isArray(tree.tree)&&tree.tree.length<=30000,'CHEAT_SOURCE','The cheat repository listing is incomplete.');
  return tree.tree.filter(x=>x.type==='blob'&&hash.test(x.sha)&&Number.isSafeInteger(x.size)&&x.size>0&&x.size<=4*1048576&&
    /^json\/(?:PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}_[a-z0-9._-]+\.json$/i.test(x.path)&&titles.has(x.path.slice(5,14)));
}
export function verifyCheatBlob(bytes,expected){
  const input=Buffer.concat([Buffer.from('blob '+bytes.length+'\0'),bytes]);
  invariant(hash.test(expected)&&Buffer.from(sha1(input)).toString('hex')===expected,'CHEAT_SOURCE_HASH','The cheat download differs from its repository file.');
}
export function installCheatSources(runtime,{readFile=fs.readFile}={}){
  runtime.jobs.register('cheat.sync',{
    validate:()=>({}),resources:()=>['native-download','cheat-catalog'],timeout:15*60*1000,cancellable:true,
    async run(_input,context){
      invariant(runtime.downloads&&runtime.cheats,'CHEAT_SOURCE','Cheat downloads are unavailable.');
      await runtime.library?.refresh();
      const titles=new Set((runtime.libraryState?.games||[]).map(x=>x.titleId));if(runtime.game)titles.add(runtime.game.titleId);
      invariant(titles.size,'CHEAT_SOURCE','No installed games found. Refresh Games, then try again.');
      let imported=0,existing=0,unsupported=0,checked=0;const failures=[];
      async function download(url,limit){const file=await runtime.downloads.fetchToFile(url,limit,context);try{return await readFile(file.source);}finally{file.release();}}
      for(const repo of sources){
        try{
          const ref=JSON.parse((await download('https://api.github.com/repos/'+repo+'/git/ref/heads/main',65536)).toString('utf8'));
          invariant(ref.object?.type==='commit'&&hash.test(ref.object.sha),'CHEAT_SOURCE','Repository revision is invalid.');
          const commit=ref.object.sha;
          const tree=JSON.parse((await download('https://api.github.com/repos/'+repo+'/git/trees/'+commit+'?recursive=1',8*1048576)).toString('utf8'));
          const files=selectCheatFiles(tree,titles);invariant(files.length<=256,'CHEAT_SOURCE_LIMIT','More than 256 cheat files match these games. Import individual files instead.');
          for(const item of files){
            invariant(!context.signal?.aborted,'DOWNLOAD_CANCELLED','Cheat update cancelled.');
            const bytes=await download('https://raw.githubusercontent.com/'+repo+'/'+commit+'/'+item.path,4*1048576);verifyCheatBlob(bytes,item.sha);
            const before=runtime.cheats.packs.list().length;
            try{await runtime.cheats.packs.import({source:bytes.toString('utf8'),titleId:item.path.slice(5,14)});
              if(runtime.cheats.packs.list().length>before)imported++;else existing++;
            }catch(error){if(error.code==='CHEAT_REGISTRY_FULL')throw error;unsupported++;}
            checked++;await context.progress({completed:checked,total:checked+files.length,phase:'Importing cheats for installed games'});
          }
        }catch(error){if(context.signal?.aborted||error.code==='DOWNLOAD_CANCELLED')return {cancelled:true,imported,message:'Cheat update cancelled. Already imported files remain available.'};failures.push(repo+': '+error.message);}
      }
      invariant(checked||!failures.length,'CHEAT_SOURCE',failures.join(' · '));
      return {completed:true,imported,existing,unsupported,failures,message:`${imported} cheat files added, ${existing} already available`+(unsupported?`, ${unsupported} unsupported files skipped`:'')+(failures.length?'. '+failures.join(' · '):'.')};
    },verify:async result=>result?.completed===true,
    async reconcile(){return {absent:true};}
  });
}
