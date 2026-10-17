// SPDX-License-Identifier: GPL-3.0-or-later
import {spawnSync} from 'node:child_process';
import {mkdirSync,readFileSync,writeFileSync} from 'node:fs';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'..');
const zig=process.env.ZIG||path.join(root,'../ps-neighbourhood/tools/zig-x86_64-windows-0.14.1/zig.exe');
const output=path.join(root,'build/goldhen');mkdirSync(output,{recursive:true});
function run(args){const r=spawnSync(zig,args,{cwd:root,encoding:'utf8',env:{...process.env,ZIG_GLOBAL_CACHE_DIR:path.join(root,'build/zig-cache')}});if(r.status!==0)throw Error(r.stderr||r.error?.message);}
const object=path.join(output,'bridge.o'),elf=path.join(output,'bridge.elf'),binary=path.join(output,'bridge.bin');
run(['cc','-target','x86_64-linux-none','-O2','-Wall','-Wextra','-Werror','-fPIC','-ffreestanding','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-mno-red-zone','-mno-avx','-c','platform/game/goldhen-bridge.c','-o',object]);
run(['ld.lld','--no-undefined','-e','pm_goldhen_bridge','-T','platform/game/game-gate.ld',object,'-o',elf]);
run(['objcopy','-O','binary','--only-section=.text',elf,binary]);
const bytes=readFileSync(binary);if(!bytes.length||bytes.length>4096)throw Error('GoldHEN gate exceeds reservation');
writeFileSync(path.join(output,'bridge.h'),'// Generated; do not edit.\nstatic const unsigned char pm_goldhen_bridge_bytes[]={'+[...bytes].join(',')+'};\n');
console.log(`Built private GoldHEN compatibility gate: ${bytes.length} bytes.`);
