// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { invariant } from './errors.mjs';
export class FileStorage {
  constructor(root) { this.root = path.resolve(root); }
  file(name) {
    invariant(/^[a-z0-9][a-z0-9.-]{0,80}$/.test(name) && !name.includes('..'), 'BAD_STATE_PATH', 'Invalid state file.');
    return path.join(this.root, name);
  }
  async init() {
    await fs.mkdir(this.root, { recursive: true, mode: 0o700 });
    invariant(!(await fs.lstat(this.root)).isSymbolicLink(), 'BAD_STATE_PATH', 'State directory cannot be a symbolic link.');
    return this;
  }
  async read(name) {
    let file;
    try {
      const target = this.file(name), stat = await fs.lstat(target);
      invariant(stat.isFile() && !stat.isSymbolicLink() && stat.size <= 4 * 1024 * 1024,
        'BAD_STATE_FILE', 'Invalid state snapshot.');
      file = await fs.open(target, 'r');
      return await file.readFile('utf8');
    } catch (error) { if (error.code === 'ENOENT') return null; throw error; }
    finally { await file?.close(); }
  }
  async replace(name, text) {
    const target = this.file(name), temporary = this.file(name + '.new');
    // A previous process may have left a torn temporary file. Never follow links.
    try { await fs.unlink(temporary); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    let file;
    try {
      file = await fs.open(temporary, 'wx', 0o600);
      await file.writeFile(text); await file.sync(); await file.close(); file = null;
      await fs.rename(temporary, target);
      if (process.platform !== 'win32') {
        const directory = await fs.open(this.root, 'r');
        try { await directory.sync(); } finally { await directory.close(); }
      }
    } finally { await file?.close(); }
  }
}
