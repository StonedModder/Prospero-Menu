// SPDX-License-Identifier: GPL-3.0-or-later
// Collect a tester-ready release folder from the build outputs:
//
//   release/ProsperoMenu-v<version>/
//     ProsperoMenu.elf                 send this to the ELF loader
//     logo.png                         the default header logo
//     overlays/<id>/overlay.json       every bundled overlay, as installed on
//     overlays/<id>/<id>.ovl           the console in /data/ProsperoMenu/overlays
//     README.txt  SHA256SUMS.txt
//
// Run after `npm run build:overlays` and `npm run build`.
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { BUILTIN_SEEDS } from '../../src/overlays/seeds.mjs';

const root = path.resolve(import.meta.dirname, '../..');
const { version } = JSON.parse(await fs.readFile(path.join(root, 'package.json'), 'utf8'));
const name = `ProsperoMenu-v${version}`, out = path.join(root, 'release', name);
await fs.rm(out, { recursive: true, force: true });
await fs.mkdir(path.join(out, 'overlays'), { recursive: true });

await fs.copyFile(path.join(root, 'build/prosperomenu-bundled.elf'), path.join(out, 'ProsperoMenu.elf'));
await fs.copyFile(path.join(root, 'assets/prosperomenu-logo.png'), path.join(out, 'logo.png'));

// Program overlays from the build, then the overlays written by the menu itself.
const built = path.join(root, 'build/overlays');
for (const id of (await fs.readdir(built)).sort()) {
  await fs.cp(path.join(built, id), path.join(out, 'overlays', id), { recursive: true, filter: file => !file.endsWith('.log') });
}
for (const seed of BUILTIN_SEEDS) {
  const folder = path.join(out, 'overlays', seed.id);
  await fs.mkdir(folder, { recursive: true });
  for (const [file, data] of Object.entries(seed.files)) await fs.writeFile(path.join(folder, file), data);
}

await fs.writeFile(path.join(out, 'README.txt'), `ProsperoMenu ${version}
Created by StonedModder

Install
  1. Jailbreak the PS5 and start an ELF loader (port 9021).
  2. Send ProsperoMenu.elf to the loader.
  3. Wait for the ProsperoMenu notification, then hold L3 + R3.
     Circle goes back, L3 + R3 closes the menu.

On first start ProsperoMenu creates /data/ProsperoMenu/, installs every overlay in
the overlays folder below and writes logo.png. You do not need to copy anything.
To replace a running ProsperoMenu, restart the console first.

Overlays
  Each folder in overlays/ is one entry of the menu, exactly as it is installed in
  /data/ProsperoMenu/overlays/ on the console. To add or update one by hand, copy
  its folder there over FTP and reopen the menu. <id>.ovl files are the overlay
  programs; overlay.json describes each entry.

Logo
  /data/ProsperoMenu/logo.png is the header logo. Replace it with your own PNG,
  or delete it to show the drawn mark.

Report problems with your firmware version, what you did and what happened.
Tested on firmware 13.60.
`);

const files = [];
const walk = async dir => { for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
  const full = path.join(dir, entry.name); if (entry.isDirectory()) await walk(full); else files.push(full);
} };
await walk(out);
const sums = [];
for (const file of files.sort()) {
  const rel = path.relative(out, file).split(path.sep).join('/');
  if (rel === 'SHA256SUMS.txt') continue;
  sums.push(createHash('sha256').update(await fs.readFile(file)).digest('hex') + '  ' + rel);
}
await fs.writeFile(path.join(out, 'SHA256SUMS.txt'), sums.join('\n') + '\n');
console.log(`release folder: release/${name} (${files.length} files)`);
