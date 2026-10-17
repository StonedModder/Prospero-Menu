// SPDX-License-Identifier: GPL-3.0-or-later
// Build the example overlays in sdk/examples/<id>/ into build/overlays/<id>/
// (overlay.json + <id>.ovl), ready to copy to /data/ProsperoMenu/overlays/
// or to be seeded by `npm run build`.
//
//   node scripts/prosperomenu/build-overlays.mjs [id ...]      PS5 payloads
//   node scripts/prosperomenu/build-overlays.mjs --host [id]   PC builds for `npm run dev`
//
// Same toolchain as scripts/build.mjs: ZIG (zig cc / zig ld.lld) and
// PS5_PAYLOAD_SDK. A manifest may list extra system libraries in "libs".
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2), host = args.includes('--host'), only = args.filter(a => !a.startsWith('--'));
const sdk = path.resolve(process.env.PS5_PAYLOAD_SDK || path.join(root, '../ps-neighbourhood/tools/ps5-sdk-0.43/ps5-payload-sdk'));
const zig = process.env.ZIG || path.join(root, '../ps-neighbourhood/tools/zig-x86_64-windows-0.14.1/zig.exe');
const run = (tool, argv) => {
  const result = spawnSync(tool, argv, { cwd: root, encoding: 'utf8', env: { ...process.env, ZIG_GLOBAL_CACHE_DIR: path.join(root, 'build/zig-cache') } });
  if (result.status !== 0) throw Error(`${path.basename(tool)} ${argv[0]} failed:\n${result.error?.message || result.stderr}`);
};
const compile = ['cc', '-Os', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-target', 'x86_64-linux-none', '-U__linux__',
  '-D__FreeBSD__=11', '-D__PS5__', '-isystem', sdk + '/target/include', '-fPIE', '-fno-stack-protector', '-fno-plt', '-femulated-tls', '-nostdlib',
  '-Isdk/include'];

const examples = path.join(root, 'sdk/examples');
for (const id of (await fs.readdir(examples)).sort()) {
  if (only.length && !only.includes(id)) continue;
  const dir = path.join(examples, id);
  const manifest = JSON.parse(await fs.readFile(path.join(dir, 'overlay.json'), 'utf8'));
  if (manifest.id !== id) throw Error(`${id}: overlay.json id must match the folder name`);
  const sources = (await fs.readdir(dir)).filter(f => f.endsWith('.c')).map(f => path.join('sdk/examples', id, f));
  if (host) {
    const out = path.join(root, 'build/overlays-host', id); await fs.mkdir(out, { recursive: true });
    run(process.env.CC || 'cc', ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Isdk/include', ...sources, 'sdk/src/pmo.c', '-lpthread', '-o', path.join(out, 'overlay')]);
    console.log(`built build/overlays-host/${id}/overlay (run with PMO_ROOT=.local/ProsperoMenu)`);
    continue;
  }
  // The overlay binary is named by the manifest ("elf"), <id>.ovl by convention.
  const out = path.join(root, 'build/overlays', id), objects = [], binary = manifest.elf || id + '.ovl';
  await fs.rm(out, { recursive: true, force: true }); await fs.mkdir(out, { recursive: true });
  for (const source of [...sources, 'sdk/src/pmo.c']) {
    const object = path.join('build/overlay-objects', id, path.basename(source, '.c') + '.o');
    await fs.mkdir(path.join(root, path.dirname(object)), { recursive: true });
    run(zig, [...compile, '-c', source, '-o', object]); objects.push(object);
  }
  const libs = (manifest.libs || []).map(lib => '-l' + lib);
  run(zig, ['ld.lld', '--no-dependent-libraries', '--eh-frame-hdr', '-pie', '--hash-style=gnu', '-z', 'max-page-size=0x4000', '-T', sdk + '/ldscripts/elf_x86_64.x',
    ...objects, sdk + '/target/lib/crt1.o', '-L', sdk + '/target/lib', '--start-group', sdk + '/target/lib/libc.a', '-ldl', '--end-group',
    '-lkernel_web', '-lSceLibcInternal', '-lSceNet', ...libs, '-o', path.join(out, binary)]);
  const { libs: _libs, ...installed } = manifest;
  await fs.writeFile(path.join(out, 'overlay.json'), JSON.stringify(installed, null, 2) + '\n');
  for (const extra of await fs.readdir(dir)) if (!extra.endsWith('.c') && !extra.endsWith('.h') && extra !== 'overlay.json') await fs.copyFile(path.join(dir, extra), path.join(out, extra));
  console.log(`built build/overlays/${id}/${binary} (${(await fs.stat(path.join(out, binary))).size} bytes)`);
}
