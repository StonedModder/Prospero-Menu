// SPDX-License-Identifier: GPL-3.0-or-later
import http from 'node:http';
import { createHash } from 'node:crypto';
import { readDpiForm } from './dpi-form.mjs';
import { MAX_PACKAGE } from './package-format.mjs';
import { invariant, publicError } from './errors.mjs';
import { dpiPage } from './package-receiver-page.mjs';
export function installPackageReceiver(runtime,{createServer=http.createServer,host='0.0.0.0',port=12800}={}) {
  let server=null,enabled=false,closing=false,active=null,last=null,transition=Promise.resolve();
  const changed=()=>{runtime.sequence++;};
  const address=()=>`http://${runtime.identity.ip}:${server?.address()?.port||port}`;
  const snapshot=()=>({enabled,closing,address:address(),active:active?{...active}:null,last});
  const reply=(res,status,text,type='text/plain; charset=utf-8')=>{res.writeHead(status,{'Content-Type':type,'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});res.end(text);};
  async function handle(req,res){
    let entry=null,acceptedForm=false;
    try{
      const actual=server?.address()?.port||port,origin=`http://${req.headers.host}`;
      invariant([`${runtime.identity.ip}:${actual}`,`127.0.0.1:${actual}`,`localhost:${actual}`].includes(req.headers.host),'DPI_HOST','Use the console receiver address.',{status:403});
      const remote=req.headers.origin;
      invariant(!remote || remote===origin || remote==='https://sniperscheats.lol','DPI_ORIGIN','This website is not allowed to submit packages.',{status:403});
      if(remote){res.setHeader('Access-Control-Allow-Origin',remote);res.setHeader('Vary','Origin');}
      if(req.method==='OPTIONS'){
        res.setHeader('Access-Control-Allow-Methods','GET, POST, OPTIONS');res.setHeader('Access-Control-Allow-Headers','Content-Type');
        if(req.headers['access-control-request-private-network']==='true')res.setHeader('Access-Control-Allow-Private-Network','true');
        return reply(res,204,'');
      }
      if(req.method==='GET' && req.url==='/')return reply(res,200,dpiPage,'text/html; charset=utf-8');
      if(req.method==='GET' && req.url==='/status'){
        const jobs=runtime.jobs.snapshot().filter(j=>j.kind==='package.receive').slice(-20).map(j=>({id:j.id,name:j.args.name,status:j.status,progress:j.progress,result:j.result,error:j.error}));
        return reply(res,200,JSON.stringify({...snapshot(),jobs}),'application/json');
      }
      invariant(req.method==='POST' && ['/','/upload','/install'].includes(req.url),'DPI_ROUTE','Use the package form on this receiver.',{status:405});
      invariant(enabled && !active,'DPI_BUSY',enabled?'Another package is still uploading. Wait for it to finish.':'Package receiving is off.',{status:409});
      invariant(!runtime.graph.safe && !runtime.recovery?.pending,'DPI_STARTUP','Wait for ProsperoMenu startup to finish.');
      const upper=Number(req.headers['content-length']);
      invariant(Number.isSafeInteger(upper)&&upper>0&&upper<=MAX_PACKAGE+65536,'DPI_LENGTH','Supply the complete upload size.',{status:413});
      active={name:'Package request',bytes:0,total:upper,phase:'Receiving'};changed();
      const form=await readDpiForm(req,{maxBytes:MAX_PACKAGE,onFile:async({name})=>{
        entry=await runtime.packageInbox.begin({name,bytes:Math.min(upper,MAX_PACKAGE)});
        active.name=name;changed();let pending=Buffer.alloc(0),offset=0,aborted=false;const hash=createHash('sha256');
        async function flush(){if(!pending.length)return;await runtime.packageInbox.append(entry.id,offset,pending);offset+=pending.length;pending=Buffer.alloc(0);}
        return {
          async write(bytes){hash.update(bytes);let at=0;while(at<bytes.length){const n=Math.min(1048576-pending.length,bytes.length-at);pending=Buffer.concat([pending,bytes.subarray(at,at+n)]);at+=n;if(pending.length===1048576)await flush();}active.bytes+=bytes.length;changed();},
          async end(){await flush();entry.receivedHash=hash.digest('hex');},
          async abort(){if(aborted)return;aborted=true;pending=Buffer.alloc(0);await runtime.packageInbox.discardReceived(entry.id);entry=null;}
        };
      }});
      // These optional upstream hints require a different install ABI. Reject
      // them explicitly instead of silently changing the requested install.
      invariant(!form.fields.ex_uri && !form.fields.playgo_scenario_id && !form.fields.icon_url,'DPI_METADATA','Extended PlayGo metadata is not supported by this receiver. Send a direct PKG file or URL.');
      if(entry)await runtime.packageInbox.sealReceived(entry.id,form.file.bytes,entry.receivedHash);
      acceptedForm=true;
      const input=entry?{id:entry.id}:{url:form.fields.url,name:'received.pkg'};
      const job=await runtime.jobs.enqueue('package.receive',{...input,contentId:form.fields.content_id||''},'dpi_'+runtime.id());
      last={job:job.id,name:entry?.name||'Package URL',message:'Accepted for verification and background installation.'};changed();
      res.setHeader('X-ProsperoMenu-Job',job.id);
      return reply(res,202,`SUCCESS: Package request accepted. Verification and installation continue in the background. Job: ${job.id}`);
    }catch(error){
      if(entry&&!acceptedForm){try{await runtime.packageInbox.discardReceived(entry.id);}catch{/* Preserve unexpected staging state for inspection. */}}
      last={error:publicError(error)};changed();return reply(res,error.status||500,'FAILED: '+publicError(error).message);
    }finally{
      // GET/status requests must not clear another connection's upload state.
      if(req.method==='POST' && active && active.owner===req)active=null;
      if(entry || acceptedForm)runtime.packageItems=await runtime.packageInbox.list();changed();
    }
  }
  // Associate each active transfer with its request without publishing it.
  async function dispatch(req,res){
    const was=active;const work=handle(req,res);if(!was&&active)Object.defineProperty(active,'owner',{value:req});
    try{await work;}finally{if(active?.owner===req){active=null;changed();}if(closing&&!server&&!active){closing=false;changed();}}
  }
  async function setEnabled(value){
    invariant(typeof value==='boolean','DPI_SETTING','Choose whether package receiving is on or off.');
    const operation=transition.catch(()=>{}).then(async()=>{
      if(value===enabled)return snapshot();
      if(!value){enabled=false;closing=Boolean(server);const old=server;old?.close(()=>{if(server===old)server=null;closing=Boolean(active);changed();});changed();return snapshot();}
      invariant(!closing,'DPI_CLOSING','The last upload is finishing; wait before restarting the receiver.');
      invariant(runtime.capabilities.get('packages')?.available,'PACKAGE_UNAVAILABLE','Package installation is unavailable.');
      const next=createServer({maxRequestBodyBytes:MAX_PACKAGE+65536},dispatch);server=next;
      try{await new Promise((resolve,reject)=>{next.once('error',reject);next.listen(port,host,resolve);});enabled=true;}
      catch(error){server=null;throw error;}
      next.on('error',error=>{last={error:publicError(error)};changed();});changed();return snapshot();
    });transition=operation;return operation;
  }
  runtime.packageReceiver={snapshot,setEnabled,close(){enabled=false;server?.closeAllConnections?.();server?.close();server=null;closing=false;}};
  return runtime.packageReceiver;
}
