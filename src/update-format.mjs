// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { invariant } from './errors.mjs';
import { inspectPayload } from './payload-format.mjs';
import { UPDATE_PROFILES } from './firmware-profiles.mjs';
export { UPDATE_PROFILES } from './firmware-profiles.mjs';
export const UPDATE_HEADER = 256, UPDATE_MAX = 64 * 1024 * 1024;
// V1 has 14 bits at offset 24. V2 retains those identities and extends the
// bitmap using the first eight reserved bytes, without changing header size.
const maskOffsets=[24,160,164];
const allowedMasks=maskOffsets.map((_,word)=>UPDATE_PROFILES.reduce((mask,fw,i)=>
  (i>>>5)===word?(mask|(1<<(i&31)))>>>0:mask,0));
const digest = bytes => createHash('sha256').update(bytes).digest();
export function parseUpdateHeader(input, { totalBytes, firmware } = {}) {
  const h = Buffer.from(input).subarray(0,UPDATE_HEADER);
  invariant(h.length===UPDATE_HEADER && h.subarray(0,8).equals(Buffer.from('TACUPD01')) && [1,2].includes(h.readUInt32LE(8)) && h.readUInt32LE(12)===UPDATE_HEADER,
    'UPDATE_FORMAT','Choose a ProsperoMenu update package produced by the build tools.');
  const version=h.readUInt32LE(8),bytes=h.readBigUInt64LE(16),labelEnd=h.indexOf(0,96);
  const masks=maskOffsets.map((at,i)=>version===1&&i?0:h.readUInt32LE(at));
  const validMask=version===1?(masks[0]>0&&(masks[0]&~0x3fff)===0):
    masks.some(Boolean)&&masks.every((mask,i)=>(mask&~allowedMasks[i])===0);
  invariant(bytes>=64n&&bytes<=BigInt(UPDATE_MAX)&&validMask&&h.readUInt32LE(28)===1&&
    h.subarray(32,64).some(x=>x!==0)&&h.subarray(64,96).some(x=>x!==0)&&
    labelEnd>96&&labelEnd<160&&h.subarray(96,labelEnd).every(x=>x>=32&&x<=126)&&h.subarray(labelEnd,160).every(x=>x===0)&&
    h.subarray(version===1?160:168,224).every(x=>x===0)&&
    digest(h.subarray(0,224)).equals(h.subarray(224)), 'UPDATE_HEADER','The update header is invalid or incompatible.');
  invariant(totalBytes==null||Number(bytes)+UPDATE_HEADER===totalBytes,'UPDATE_SIZE','The update package length does not match its header.');
  const profiles=UPDATE_PROFILES.filter((_,i)=>masks[i>>>5]&(1<<(i&31)));
  invariant(!firmware||profiles.includes(firmware),'UPDATE_FIRMWARE','This update does not declare your exact firmware profile.');
  return {format:'prosperomenu',formatVersion:version,bytes:Number(bytes),sha256:h.subarray(32,64).toString('hex'),source:h.subarray(64,96).toString('hex'),label:h.subarray(96,labelEnd).toString('ascii'),profiles,stateSchema:1};
}
export function makeUpdatePackage(elf,{source,label,profiles=UPDATE_PROFILES}) {
  elf=Buffer.from(elf);inspectPayload(elf.subarray(0,16384),'prosperomenu-runtime.elf',{totalBytes:elf.length});
  invariant(elf.length<=UPDATE_MAX&&/^[a-f0-9]{64}$/.test(source)&&typeof label==='string'&&/^[\x20-\x7e]{1,63}$/.test(label)&&
    Array.isArray(profiles)&&profiles.length&&new Set(profiles).size===profiles.length&&profiles.every(fw=>UPDATE_PROFILES.includes(fw)), 'UPDATE_BUILD','Invalid update build metadata.');
  const version=profiles.some(fw=>UPDATE_PROFILES.indexOf(fw)>=14)?2:1;
  const h=Buffer.alloc(UPDATE_HEADER);h.write('TACUPD01');h.writeUInt32LE(version,8);h.writeUInt32LE(UPDATE_HEADER,12);h.writeBigUInt64LE(BigInt(elf.length),16);
  for(const fw of profiles){const i=UPDATE_PROFILES.indexOf(fw),at=maskOffsets[i>>>5];h.writeUInt32LE((h.readUInt32LE(at)|(1<<(i&31)))>>>0,at);}
  h.writeUInt32LE(1,28);
  digest(elf).copy(h,32);Buffer.from(source,'hex').copy(h,64);h.write(label,96,'ascii');digest(h.subarray(0,224)).copy(h,224);
  parseUpdateHeader(h,{totalBytes:h.length+elf.length});return Buffer.concat([h,elf]);
}
export function inspectUpdate(input, options={}) {
  const bytes=Buffer.from(input),header=parseUpdateHeader(bytes,{...options,totalBytes:bytes.length}),elf=bytes.subarray(UPDATE_HEADER);
  invariant(digest(elf).toString('hex')===header.sha256,'UPDATE_CHECKSUM','The update payload checksum does not match.');
  inspectPayload(elf.subarray(0,16384),'prosperomenu-runtime.elf',{totalBytes:elf.length});return header;
}
