// SPDX-License-Identifier: GPL-3.0-or-later
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
const root = path.resolve(import.meta.dirname, '..');
const zig = process.env.ZIG || path.join(root, '../ps-neighbourhood/tools/zig-x86_64-windows-0.14.1/zig.exe');
const output = path.resolve(root, process.argv[2] || 'build/pad');
if (!output.startsWith(path.join(root, 'build') + path.sep)) throw Error('Pad bridge output must stay in this checkout’s build directory');
mkdirSync(output, { recursive: true });
function run(args) {
  const r = spawnSync(zig, args, { cwd: root, encoding: 'utf8', env: { ...process.env, ZIG_GLOBAL_CACHE_DIR: path.join(root, 'build/zig-cache') } });
  if (r.status !== 0) throw Error(r.stderr || r.error?.message || 'Pad bridge build failed');
}
run(['cc', '-target', 'x86_64-linux-none', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-ffreestanding',
  '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-mno-red-zone', '-mno-avx',
  '-c', 'platform/game/pad-bridge.c', '-o', path.join(output, 'pad.o')]);
run(['ld.lld', '--no-undefined', '-T', 'platform/game/pad-bridge.ld', path.join(output, 'pad.o'), '-o', path.join(output, 'pad.elf')]);
run(['objcopy', '-O', 'binary', '--only-section=.text', path.join(output, 'pad.elf'), path.join(output, 'pad.bin')]);
const bytes = readFileSync(path.join(output, 'pad.bin'));
if (!bytes.length || bytes.length > 8192) throw Error('Pad bridge exceeds its reservation');
writeFileSync(path.join(output, 'pad.h'), '// Generated; do not edit.\nstatic const unsigned char pm_pad_bridge_bytes[]={' + [...bytes].join(',') + '};\n');
writeFileSync(path.join(output, 'manifest.json'), JSON.stringify({ bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex'), hardwareVerified: false }, null, 2));
console.log(`Built native post-read pad bridge: ${bytes.length} bytes; hardware untested.`);
