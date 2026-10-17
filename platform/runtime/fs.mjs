// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from PS Neighborhood platform plumbing; see vendor/runtime-provenance.json.
import path from 'path-browserify';
import {Readable,Writable} from 'readable-stream';
// Native functions also enforce this root and reject symlink traversal. This
// check makes accidental desktop paths produce a useful error before syscalls.
export const ROOT='/data/ProsperoMenu';
// ProsperoMenu's own folder (settings and user overlays) is the second
// writable root; host.c applies the same rule natively.
export const PM_ROOT='/data/ProsperoMenu';
// The ps5_autoloader folders (internal and each USB) are the third writable
// workspace: the Autoload config page edits their autoload.txt.
export const AUTOLOAD_ROOTS=['/data/ps5_autoloader',...[0,1,2,3,4,5,6,7].map(n=>'/mnt/usb'+n+'/ps5_autoloader')];
const WRITE_ROOTS=[ROOT,PM_ROOT,...AUTOLOAD_ROOTS];
const rootOf=p=>WRITE_ROOTS.find(r=>p===r||p.startsWith(r+'/'));
function safe(p){p=path.resolve(String(p));if(!rootOf(p))throw Error('Choose a file in the console workspace');return p;}
function readable(p){p=path.resolve(String(p));if(p==='/data'||p.startsWith('/data/')||p==='/user'||p.startsWith('/user/')||/^\/mnt\/usb[0-7](?:\/|$)/.test(p))return p;return safe(p);}
function info(s){return {...s,isFile:()=>s.kind===1,isDirectory:()=>s.kind===2,isSymbolicLink:()=>s.kind===3};}
export async function stat(p){return info(__native.stat(readable(p)));}
export const lstat=stat;
export async function statfs(p){return __native.statfs(readable(p));}
export async function realpath(p){return __native.realpath(readable(p));}
export async function mkdir(p,options={}){p=safe(p);if(options.recursive){const root=rootOf(p);try{__native.mkdir(root);}catch(e){if(e.code!=='EEXIST')throw e;}const relative=p.slice(root.length);let current=root;for(const segment of relative.split('/').filter(Boolean)){current+='/'+segment;try{__native.mkdir(current);}catch(e){if(e.code!=='EEXIST')throw e;}}}else __native.mkdir(p);}
export async function readdir(p,options={}){return __native.readdir(readable(p)).map(e=>options.withFileTypes?{...info(e),name:e.name}:e.name);}
export async function rename(a,b){__native.rename(safe(a),safe(b));}
export async function unlink(p){__native.unlink(safe(p));}
export async function rm(p,options={}){p=safe(p);if(p===ROOT||p===PM_ROOT)throw Error('Cannot remove workspace root');let s;try{s=await stat(p);}catch(e){if(options.force&&e.code==='ENOENT')return;throw e;}if(s.isDirectory()){if(!options.recursive)throw Error('Recursive option required');for(const e of await readdir(p))await rm(path.join(p,e),options);__native.rmdir(p);}else __native.unlink(p);}
class FileHandle{
 constructor(fd){this.fd=fd;this.closed=false;}
 check(){if(this.closed)throw Error('File closed');}
 async read(buffer,offset=0,length=buffer.length-offset,position=null){this.check();const b=Buffer.from(__native.read(this.fd,Math.min(length,1048576),position));b.copy(buffer,offset);return {bytesRead:b.length,buffer};}
 async write(buffer,offset=0,length,position=null){this.check();if(typeof buffer==='string')buffer=Buffer.from(buffer);length??=buffer.length-offset;const bytesWritten=__native.write(this.fd,buffer.subarray(offset,offset+Math.min(length,1048576)),position);return {bytesWritten,buffer};}
 async writeFile(data){if(typeof data==='string')data=Buffer.from(data);for(let n=0;n<data.length;){const {bytesWritten}=await this.write(data,n);if(!bytesWritten)throw Error('Short write');n+=bytesWritten;}}
 async readFile(encoding){const chunks=[];let total=0;for(;;){const b=Buffer.from(__native.read(this.fd,65536,null));if(!b.length)break;total+=b.length;if(total>64*1048576)throw Error('Use streaming for files larger than 64 MiB');chunks.push(b);}const b=Buffer.concat(chunks);return encoding?b.toString(typeof encoding==='string'?encoding:encoding.encoding):b;}
 async stat(){this.check();return info(__native.fstat(this.fd));}
 async sync(){this.check();__native.fsync(this.fd);}
 async close(){if(!this.closed){this.closed=true;__native.close(this.fd);}}
 createReadStream(options={}){return streamRead(this,options);}
}
export async function open(p,flags='r',mode=0o600){return new FileHandle(__native.open(flags==='r'?readable(p):safe(p),flags,mode));}
export async function readFile(p,encoding){const f=await open(p);try{return await f.readFile(encoding);}finally{await f.close();}}
export async function writeFile(p,data,options={}){const f=await open(p,options.flag||'w',options.mode);try{await f.writeFile(typeof data==='string'?Buffer.from(data,options.encoding||'utf8'):data);}finally{await f.close();}}
function streamRead(file,options={}){let f,offset=options.start??0,pending=false;const stream=new Readable({highWaterMark:options.highWaterMark||65536,async read(size){if(pending)return;pending=true;try{f??=await file;const count=Math.min(size,1048576,options.end==null?Infinity:Math.max(0,options.end-offset+1));if(!count){this.push(null);return;}const b=Buffer.alloc(count),{bytesRead}=await f.read(b,0,count,offset);offset+=bytesRead;this.push(bytesRead?b.subarray(0,bytesRead):null);}catch(e){this.destroy(e);}finally{pending=false;}},destroy(error,callback){Promise.resolve(f||file).then(h=>options.autoClose===false?null:h.close()).then(()=>callback(error),callback);}});return stream;}
export function createReadStream(p,options={}){return streamRead(options.fd!=null?new FileHandle(options.fd):open(p,options.flags||'r'),options);}
export function createWriteStream(p,options={}){const file=options.fd!=null?Promise.resolve(new FileHandle(options.fd)):open(p,options.flags||'w',options.mode);return new Writable({write(b,_e,cb){file.then(f=>f.writeFile(b)).then(()=>cb(),cb);},final(cb){file.then(f=>options.autoClose===false?null:f.close()).then(()=>cb(),cb);},destroy(e,cb){file.then(f=>options.autoClose===false?null:f.close()).then(()=>cb(e),cb);}});}
export const constants={F_OK:0,R_OK:4,W_OK:2};
export function access(p,_mode,callback){if(typeof _mode==='function')callback=_mode;stat(p).then(()=>callback(null),callback);}
export const promises={stat,lstat,statfs,realpath,mkdir,readdir,rename,unlink,rm,open,readFile,writeFile};
export default {...promises,createReadStream,createWriteStream,constants,access,promises};

// Used only by the explicit file-browser job. General filesystem APIs keep their workspace-only write policy.
export const fileManagerIO={stat,lstat,statfs,readdir,
 async open(p,flags='r'){return flags==='r'?open(p,flags):new FileHandle(__native.browserOpen(readable(p),flags));},
 async unlink(p){__native.browserUnlink(readable(p));},
 async mkdir(p){__native.browserMkdir(readable(p));},
 async rmdir(p){__native.browserRmdir(readable(p));}
};
