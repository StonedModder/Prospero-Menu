// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from PS Neighborhood platform plumbing; see vendor/runtime-provenance.json.
import {Duplex} from 'readable-stream';
import {EventEmitter} from 'events';
export function isIP(s){return typeof s==='string'&&/^\d+\.\d+\.\d+\.\d+$/.test(s)&&s.split('.').every(n=>Number(n)<=255)?4:0;}
const sockets=new Set(),servers=new Set();
export class Socket extends Duplex{
 constructor(options={}){super({allowHalfOpen:options.allowHalfOpen??false});this.fd=options.fd??-1;this.connecting=false;this.readReady=false;this.pending=null;this.lastActivity=Date.now();this.remoteAddress='127.0.0.1';this.remoteFamily='IPv4';this.localAddress='127.0.0.1';if(this.fd>=0){this.remoteAddress=__native.peer(this.fd);sockets.add(this);}}
 connect(options,host,listener){if(typeof options==='number')options={port:options,host:typeof host==='string'?host:'127.0.0.1'};else listener=host;options.host??='127.0.0.1';if(options.host!=='127.0.0.1')throw Error('Local console services must use loopback');if(listener)this.once('connect',listener);this.connecting=true;this.remotePort=options.port;try{this.fd=__native.connect(options.port);sockets.add(this);}catch(e){queueMicrotask(()=>this.destroy(e));}return this;}
 setNoDelay(){return this;} setKeepAlive(){return this;}
 setTimeout(ms,cb){this.timeout=ms;this.lastActivity=Date.now();if(cb)this.once('timeout',cb);return this;}
 address(){return {address:this.localAddress,port:0,family:'IPv4'};}
 _read(){this.readReady=true;}
 _write(data,_encoding,callback){this.pending={data,offset:0,callback};}
 _final(callback){try{if(this.fd>=0)__native.shutdown(this.fd);callback();}catch(e){callback(e);}}
 _destroy(error,callback){sockets.delete(this);if(this.fd>=0){__native.close(this.fd);this.fd=-1;}const p=this.pending;this.pending=null;p?.callback(error||Error('Socket closed'));callback(error);}
 poll(){try{if(this.connecting){const ready=__native.connected(this.fd);if(!ready)return;this.connecting=false;this.emit('connect');}const p=this.pending;if(p){const n=__native.send(this.fd,p.data.subarray(p.offset,p.offset+65536));if(n>0){p.offset+=n;this.lastActivity=Date.now();}if(p.offset===p.data.length){this.pending=null;p.callback();}}if(this.readReady){const b=__native.recv(this.fd,65536);if(b!==null){if(b.byteLength){this.lastActivity=Date.now();this.readReady=this.push(Buffer.from(b));}else{this.readReady=false;this.push(null);}}}if(this.timeout&&Date.now()-this.lastActivity>=this.timeout){this.lastActivity=Date.now();this.emit('timeout');}}catch(e){this.destroy(e);}}
}
export function createConnection(options,listener){return new Socket().connect(options,listener);}
export const connect=createConnection;
export class Server extends EventEmitter{
 constructor(listener){super();if(listener)this.on('connection',listener);this.fd=-1;this.connections=new Set();}
 listen(port,host,listener){if(typeof host==='function'){listener=host;host='0.0.0.0';}try{this.fd=__native.listen(port,host||'0.0.0.0');this.port=__native.port(this.fd);this.listening=true;servers.add(this);queueMicrotask(()=>{this.emit('listening');listener?.();});}catch(e){queueMicrotask(()=>this.emit('error',e));}return this;}
 address(){return {address:'0.0.0.0',port:this.port,family:'IPv4'};}
 close(callback){this.listening=false;servers.delete(this);if(this.fd>=0)__native.close(this.fd);this.fd=-1;callback?.();this.emit('close');}
 closeAllConnections(){for(const s of this.connections)s.destroy();}
 poll(){for(let n=0;n<4;n++){const fd=__native.accept(this.fd);if(fd<0)break;if(this.connections.size>=24){__native.close(fd);continue;}const s=new Socket({fd});this.connections.add(s);s.once('close',()=>this.connections.delete(s));this.emit('connection',s);}}
}
export function createServer(listener){return new Server(listener);}
setInterval(()=>{for(const s of servers)s.poll();for(const s of sockets)s.poll();},10);
export default {Socket,Server,isIP,createConnection,connect,createServer};
