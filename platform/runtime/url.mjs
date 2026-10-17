// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from PS Neighborhood platform plumbing; see vendor/runtime-provenance.json.
export function fileURLToPath(url){return String(url).replace(/^file:\/\//,'');}
export class URL{
 constructor(value,base){let s=String(value);if(!/^[a-z]+:\/\//i.test(s)){const b=typeof base==='string'?new URL(base):base;if(!b)throw Error('Relative URL without base');s=b.origin+(s.startsWith('/')?s:b.pathname.slice(0,b.pathname.lastIndexOf('/')+1)+s);}const m=/^([a-z]+:)\/\/([^/]*)(\/[^?#]*)?(\?[^#]*)?(#.*)?$/i.exec(s);if(!m)throw Error('Invalid URL');this.protocol=m[1];this.host=m[2];this.hostname=this.host.split(':')[0];this.port=this.host.split(':')[1]||'';this.origin=this.protocol+'//'+this.host;this.pathname=m[3]||'/';this.search=m[4]||'';this.hash=m[5]||'';this.href=this.origin+this.pathname+this.search+this.hash;this.searchParams=new URLSearchParams(this.search);}
 toString(){return this.href;}
}
export class URLSearchParams{constructor(s=''){this.entries=String(s).replace(/^\?/,'').split('&').filter(Boolean).map(p=>{const at=p.indexOf('=');return [decodeURIComponent(at<0?p:p.slice(0,at)),decodeURIComponent((at<0?'':p.slice(at+1)).replace(/\+/g,' '))];});}get(k){return this.entries.find(e=>e[0]===k)?.[1]??null;}}
globalThis.URL=URL;globalThis.URLSearchParams=URLSearchParams;
