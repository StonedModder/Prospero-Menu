// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from PS Neighborhood platform plumbing; see vendor/runtime-provenance.json.
import {Buffer} from 'buffer';
// QuickJS has no browser-provided AbortController. Import the implementation,
// not the package's browser entry (which merely re-exports window/self).
import {AbortController,AbortSignal} from 'abort-controller/dist/abort-controller.mjs';
globalThis.Buffer=Buffer;
globalThis.AbortController=AbortController;
globalThis.AbortSignal=AbortSignal;
const timers=new Map();let sequence=0;
globalThis.setTimeout=(fn,delay=0,...args)=>{const timer={id:++sequence,at:Date.now()+Math.max(0,Number(delay)||0),fn:()=>fn(...args),unref(){return this;},ref(){return this;}};timers.set(timer.id,timer);return timer;};
globalThis.clearTimeout=t=>timers.delete(t?.id??t);
globalThis.setInterval=(fn,delay,...args)=>{const timer=setTimeout(fn,delay,...args);timer.interval=Math.max(1,delay);return timer;};
globalThis.clearInterval=clearTimeout;
globalThis.setImmediate=(fn,...args)=>setTimeout(fn,0,...args);
globalThis.clearImmediate=clearTimeout;
globalThis.queueMicrotask=fn=>Promise.resolve().then(fn);
globalThis.__tick=()=>{const now=Date.now();for(const t of [...timers.values()])if(t.at<=now&&timers.has(t.id)){if(t.interval)t.at=now+t.interval;else timers.delete(t.id);t.fn();}};
globalThis.process={env:{},platform:'freebsd',argv:[],version:'v24.0.0',versions:{},nextTick:queueMicrotask,cwd:()=>'/data/ProsperoMenu',on(){},emitWarning:m=>console.warn(m)};
process.nextTick=(fn,...args)=>queueMicrotask(()=>fn(...args));
globalThis.console={log:(...a)=>__native.log(a.map(String).join(' ')),warn:(...a)=>__native.log(a.map(String).join(' ')),error:(...a)=>__native.log(a.map(String).join(' '))};
globalThis.TextEncoder=class{encode(text=''){return Uint8Array.from(Buffer.from(text,'utf8'));}};
globalThis.TextDecoder=class{constructor(encoding='utf8'){this.encoding=encoding;}decode(data){return Buffer.from(data||[]).toString(this.encoding);}};
AbortSignal.timeout=ms=>{const c=new AbortController();setTimeout(()=>c.abort(),ms);return c.signal;};
AbortSignal.prototype.throwIfAborted=function(){if(this.aborted)throw Error('Operation cancelled');};
globalThis.performance={now:()=>Date.now()};
