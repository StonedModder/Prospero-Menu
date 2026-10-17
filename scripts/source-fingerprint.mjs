// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';

// Source identity, not a claim of reproducible SDK/compiler output. The final
// artifact has its separate ELF hash in build/manifest.json.
export async function sourceFingerprint(root) {
  const files = ['package.json', 'package-lock.json'];
  async function visit(directory) {
    for (const entry of await fs.readdir(path.join(root, directory), { withFileTypes: true })) {
      if (entry.name.startsWith('.') || entry.isSymbolicLink()) continue;
      const name = directory + '/' + entry.name;
      if (entry.isDirectory()) await visit(name);
      else if (/\.(?:mjs|js|json|py|c|h|cpp|hpp|S|inc|ld|html|css|md|txt)$/.test(name)) files.push(name);
    }
  }
  for (const directory of ['src', 'platform', 'scripts', 'vendor']) await visit(directory);
  const hash = createHash('sha256');
  for (const file of files.sort()) {
    // Git archives use LF even when a Windows checkout uses CRLF. Hash the
    // canonical text so the archived source retains the same build identity.
    const bytes = Buffer.from((await fs.readFile(path.join(root, file), 'utf8')).replace(/\r\n/g, '\n'), 'utf8');
    hash.update(file + '\0' + bytes.length + '\0');hash.update(bytes);
  }
  return hash.digest('hex');
}
