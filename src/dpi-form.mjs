// SPDX-License-Identifier: GPL-3.0-or-later
// DPI v2 uses ordinary urlencoded or multipart fields. File bytes are streamed
// into a caller-owned staging sink; parsing never submits an installation.
import { invariant } from './errors.mjs';
const allowed = new Set(['url','content_name','content_id','playgo_scenario_id','ex_uri','icon_url']);
const fail = (ok,message) => invariant(ok,'DPI_FORM',message,{status:400});
function fieldsObject(entries) {
  const fields = Object.create(null);
  for (const [key,value] of entries) {
    fail(allowed.has(key) && !(key in fields),'Unknown or duplicate package field.');
    fail(Buffer.byteLength(value)<=8192 && !/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(value),'Invalid package field.');
    fields[key]=value;
  }
  return fields;
}
export async function readDpiForm(request,{onFile,maxBytes=1024**4}={}) {
  const type=request.headers['content-type'] || '';
  if (/^application\/x-www-form-urlencoded(?:\s*;|$)/i.test(type)) {
    const chunks=[];let size=0;
    for await(const chunk of request) {size+=chunk.length;fail(size<=32768,'Package URL request is too large.');chunks.push(chunk);}
    const text=Buffer.concat(chunks).toString('utf8');fail(!text.includes('\ufffd'),'Package fields must be UTF-8.');
    const entries=text.split('&').filter(Boolean).map(part=>{
      const split=part.indexOf('=');fail(split>0,'Invalid URL-encoded package field.');
      try{return [part.slice(0,split),part.slice(split+1)].map(x=>decodeURIComponent(x.replace(/\+/g,' ')));}
      catch{fail(false,'Invalid URL encoding in package fields.');}
    });
    const fields=fieldsObject(entries);fail(Boolean(fields.url),'Supply a package URL.');return {fields,file:null};
  }
  const match=/^multipart\/form-data\s*;\s*boundary=(?:"([0-9A-Za-z'()+_,.\/:=? -]{1,70})"|([0-9A-Za-z'()+_,.\/:=?-]{1,70}))\s*$/i.exec(type);
  fail(match,'Use a package URL form or multipart file upload.');
  const boundary=match[1]||match[2];fail(!boundary.endsWith(' '),'Invalid multipart boundary.');
  const first=Buffer.from('--'+boundary+'\r\n'),marker=Buffer.from('\r\n--'+boundary);
  let buffer=Buffer.alloc(0),phase='first',part=null,parts=0,total=0,fieldBytes=0,file=null,sink=null;
  const fields=[];let finished=false;
  async function data(bytes){
    if(!bytes.length)return;part.bytes+=bytes.length;
    if(part.file){fail(part.bytes<=maxBytes,'Package exceeds the receiver size limit.');await sink.write(bytes);}
    else{fieldBytes+=bytes.length;fail(fieldBytes<=32768 && part.bytes<=8192,'Package fields are too large.');part.chunks.push(Buffer.from(bytes));}
  }
  async function endPart(){
    if(part.file){file={name:part.filename,bytes:part.bytes};await sink.end();}
    else{const text=Buffer.concat(part.chunks).toString('utf8');fail(!text.includes('\ufffd'),'Package fields must be UTF-8.');fields.push([part.name,text]);}
    part=null;
  }
  async function consume(){
    for(;;){
      if(phase==='first'){
        if(buffer.length<first.length)return;fail(buffer.subarray(0,first.length).equals(first),'Invalid multipart opening boundary.');buffer=buffer.subarray(first.length);phase='headers';
      }else if(phase==='headers'){
        const end=buffer.indexOf('\r\n\r\n');if(end<0){fail(buffer.length<=8192,'Multipart headers are too large.');return;}
        fail(end<=8192 && ++parts<=8,'Too many or oversized multipart parts.');
        const headers=Object.create(null);
        for(const line of buffer.subarray(0,end).toString('utf8').split('\r\n')){
          const item=/^([a-z-]+):\s*([^\r\n]+)$/i.exec(line);fail(item && !(item[1].toLowerCase() in headers),'Invalid multipart headers.');headers[item[1].toLowerCase()]=item[2];
        }
        const disposition=/^form-data;\s*name="([a-z_]+)"(?:;\s*filename="([^"\r\n]*)")?\s*$/i.exec(headers['content-disposition']||'');
        fail(disposition,'Invalid multipart disposition.');
        const [,name,filename]=disposition;const isFile=filename!==undefined;
        if(isFile){
          fail(name==='file' && !file && !sink && typeof onFile==='function','Only one package file is accepted.');
          fail(/^[^/\\\x00-\x1f\x7f]{1,180}\.pkg$/i.test(filename) && !filename.includes('\ufffd'),'Choose a package filename ending in .pkg.');
          sink=await onFile({name:filename});fail(sink && typeof sink.write==='function' && typeof sink.end==='function','Package staging is unavailable.');
        }else fail(allowed.has(name) && !fields.some(x=>x[0]===name),'Unknown or duplicate package field.');
        part={name,filename,file:isFile,bytes:0,chunks:[]};buffer=buffer.subarray(end+4);phase='body';
      }else if(phase==='body'){
        let position=0,index=-1;
        for(;;){
          index=buffer.indexOf(marker,position);if(index<0)break;
          if(buffer.length<index+marker.length+2){if(index){await data(buffer.subarray(0,index));buffer=buffer.subarray(index);}return;}
          const suffix=buffer.subarray(index+marker.length,index+marker.length+2).toString('ascii');
          if(suffix==='--'||suffix==='\r\n')break;
          position=index+1; // A boundary-looking sequence inside binary data.
        }
        if(index<0){const safe=buffer.length-marker.length-2;if(safe>0){await data(buffer.subarray(0,safe));buffer=buffer.subarray(safe);}return;}
        await data(buffer.subarray(0,index));await endPart();const at=index+marker.length;
        phase=buffer.subarray(at,at+2).toString('ascii')==='--'?'end':'headers';buffer=buffer.subarray(at+2);
      }else{
        fail(buffer.length<=2 && (!buffer.length || buffer[0]===13 && (buffer.length===1 || buffer[1]===10)),'Unexpected data after multipart form.');return;
      }
    }
  }
  try{
    for await(const chunk of request){
      total+=chunk.length;fail(total<=maxBytes+65536,'Package upload is too large.');
      // Bound parser memory even when a caller supplies an unusually large chunk.
      for(let at=0;at<chunk.length;at+=65536){buffer=Buffer.concat([buffer,chunk.subarray(at,at+65536)]);await consume();}
    }
    fail(phase==='end' && (buffer.length===0 || buffer.length===2),'Package upload ended before its closing boundary.');
    const values=fieldsObject(fields);fail(Boolean(values.url)!==Boolean(file),'Supply either a package URL or a package file.');
    fail(!file || file.bytes>=8192,'The package file is incomplete or too small.');finished=true;return {fields:values,file};
  }finally{if(!finished)await sink?.abort?.();}
}
