// SPDX-License-Identifier: GPL-3.0-or-later
// Exact profiles and append-only update bit assignments share one source.
import { readFile } from 'node:fs/promises';
export async function firmwareProfileFiles() {
  const data = JSON.parse(await readFile(new URL('../vendor/firmware-provenance.json', import.meta.url), 'utf8'));
  const rows = data.firmwares, ordered = [...rows].sort((a,b)=>a.updateBit-b.updateBit);
  if (rows.length > 96 || new Set(rows.map(p=>p.firmware)).size !== rows.length ||
      ordered.some((p,i)=>p.updateBit!==i)) throw Error('Invalid firmware profile identity or wire mapping');
  const fields = ['allproc','security','rootvnode','textDelta','sysentvec','sysentvecPs4','cryptSingletonArray','sysents','sysentsPs4','pagerTable'];
  for (const p of rows) if (Number(p.code)!==parseInt(p.firmware.replace('.',''),16)*65536 ||
      fields.some(f=>!Number.isSafeInteger(p.offsets[f])||p.offsets[f]<=0)) throw Error('Invalid profile '+p.firmware);
  const notice = '// SPDX-License-Identifier: GPL-3.0-or-later\n// Generated from vendor/firmware-provenance.json; run node scripts/generate-firmware-profiles.mjs.\n';
  const native = notice + `// Offsets are relative to the SDK kernel DATA base, including pre-3.00 normalization.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct SnipersFirmwareProfile { uint32_t code; const char *name; ${fields.map(f=>'uintptr_t '+f+';').join(' ')} } SnipersFirmwareProfile;
static const SnipersFirmwareProfile snipers_firmware_profiles[] = {
${rows.map(p=>` {${p.code}u,"${p.firmware}",${fields.map(f=>'0x'+p.offsets[f].toString(16)).join(',')}},`).join('\n')}
};
static inline const SnipersFirmwareProfile* snipers_firmware_profile(uint32_t code) {
 code &= 0xffff0000u;
 for(size_t i=0;i<sizeof(snipers_firmware_profiles)/sizeof(snipers_firmware_profiles[0]);i++)
  if(snipers_firmware_profiles[i].code==code)return &snipers_firmware_profiles[i];
 return NULL;
}
`;
  const masks = [0,0,0]; for(const p of ordered) masks[p.updateBit>>>5]=(masks[p.updateBit>>>5]|(1<<(p.updateBit&31)))>>>0;
  const wire = notice + `// Bits 0-13 retain the Beta 1 meaning. Never reorder or reuse assigned bits.
#pragma once
#include <stdint.h>
static const uint32_t pm_update_profiles[]={${ordered.map(p=>p.code+'u').join(',')}};
static const uint32_t pm_update_masks[]={${masks.map(n=>'0x'+n.toString(16)+'u').join(',')}};
`;
  const js = notice + `export const FIRMWARES = Object.freeze(${JSON.stringify(rows.map(p=>p.firmware))});\n// Append-only wire order: the original fourteen versions retain their bit positions.\nexport const UPDATE_PROFILES = Object.freeze(${JSON.stringify(ordered.map(p=>p.firmware))});\n`;
  return new Map([['platform/native/firmware-profiles.h',native],['platform/native/update-profiles.h',wire],['src/firmware-profiles.mjs',js]]);
}
