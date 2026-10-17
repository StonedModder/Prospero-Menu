// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from PS Neighborhood platform plumbing; see vendor/runtime-provenance.json.
import {sha256} from '@noble/hashes/sha256';
export function randomBytes(n){if(!Number.isInteger(n)||n<0||n>1048576)throw Error('Invalid random size');return Buffer.from(__native.random(n));}
export function randomUUID(){const b=randomBytes(16);b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;const s=b.toString('hex');return `${s.slice(0,8)}-${s.slice(8,12)}-${s.slice(12,16)}-${s.slice(16,20)}-${s.slice(20)}`;}
export function createHash(name){
 if(name!=='sha256')throw Error('Unsupported hash: '+name);
 const native=globalThis.__native;
 if(native?.hashCreate&&native?.hashUpdate&&native?.hashDigest){
  const h=native.hashCreate();
  return {update(input){const b=typeof input==='string'?Buffer.from(input):input;
   if(!ArrayBuffer.isView(b))throw new TypeError('Expected hash bytes or text');
   for(let at=0;at<b.byteLength;at+=1048576)native.hashUpdate(h,b.buffer,b.byteOffset+at,Math.min(1048576,b.byteLength-at));
   // Preserve finalized-state checks even for an empty update.
   if(!b.byteLength)native.hashUpdate(h,b.buffer,b.byteOffset,0);return this;
  },digest(encoding){const b=Buffer.from(native.hashDigest(h));return encoding?b.toString(encoding):b;}};
 }
 const h=sha256.create();return {update(b){h.update(typeof b==='string'?Buffer.from(b):b);return this;},digest(encoding){const b=Buffer.from(h.digest());return encoding?b.toString(encoding):b;}};
}
export function timingSafeEqual(a,b){if(a.length!==b.length)throw Error('Length mismatch');let diff=0;for(let i=0;i<a.length;i++)diff|=a[i]^b[i];return diff===0;}
