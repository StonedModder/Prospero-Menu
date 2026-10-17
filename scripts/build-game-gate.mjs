// SPDX-License-Identifier: GPL-3.0-or-later
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
const root = path.resolve(import.meta.dirname, '..');
const zig = process.env.ZIG || path.join(root, '../ps-neighbourhood/tools/zig-x86_64-windows-0.14.1/zig.exe');
const output = path.resolve(root, process.argv[1]?.endsWith('build-game-gate.mjs') && process.argv[2] || 'build/game');
if (!output.startsWith(path.join(root, 'build') + path.sep)) throw Error('Game gate output must stay inside this checkout’s build directory');
mkdirSync(output, { recursive: true });
const object = path.join(output, 'gate.o'), elf = path.join(output, 'gate.elf'), binary = path.join(output, 'gate.bin');
function run(args) {
  const result = spawnSync(zig, args, { cwd: root, encoding: 'utf8', env: { ...process.env, ZIG_GLOBAL_CACHE_DIR: path.join(root, 'build/zig-cache') } });
  if (result.status !== 0) throw Error(result.stderr || result.error?.message || 'Game gate build failed');
}
run(['cc', '-target', 'x86_64-linux-none', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-ffreestanding',
  '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-mno-red-zone', '-mno-avx',
  '-c', 'platform/game/game-gate.c', '-o', object]);
run(['ld.lld', '--no-undefined', '-T', 'platform/game/game-gate.ld', object, '-o', elf]);
run(['objcopy', '-O', 'binary', '--only-section=.text', elf, binary]);
const bytes = readFileSync(binary);
if (!bytes.length || bytes.length > 4096) throw Error('Game gate exceeds its reservation');
writeFileSync(path.join(output, 'gate.h'), '// Generated; do not edit.\nstatic const unsigned char pm_game_gate_bytes[]={' + [...bytes].join(',') + '};\n');
writeFileSync(path.join(output, 'manifest.json'), JSON.stringify({ bytes: bytes.length,
  sha256: createHash('sha256').update(bytes).digest('hex'), hardwareVerified: false, injectionIntegrated: false }, null, 2));
console.log(`Built native game callback: ${bytes.length} bytes; console injection remains separate.`);
