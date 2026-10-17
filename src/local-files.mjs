// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { invariant } from './errors.mjs';
const extensions = { package: /\.pkg$/i, payload: /\.elf$/i, cheat: /\.(json|shn|xml|mc4)$/i };
export function installLocalFiles(runtime, { roots = [{ label: 'Console storage', path: '/data' },
  ...Array.from({ length: 8 }, (_, i) => ({ label: 'USB ' + (i + 1), path: '/mnt/usb' + i, requireMounted: true }))], volumeStat = fs.statfs } = {}) {
  async function resolve(source) {
    invariant(typeof source === 'string' && source.length < 1024, 'FILE_PATH', 'Choose a storage location.');
    const target = path.resolve(source), root = roots.find(r => target === path.resolve(r.path) || target.startsWith(path.resolve(r.path) + path.sep));
    invariant(root, 'FILE_PATH', 'Choose a file from console storage or USB.');
    const base = path.resolve(root.path), pieces = path.relative(base, target).split(path.sep).filter(Boolean);
    let current = base;
    for (const piece of ['', ...pieces]) {
      if (piece) current = path.join(current, piece);
      invariant(!(await fs.lstat(current)).isSymbolicLink(), 'FILE_LINK', 'Symbolic links cannot be selected.');
    }
    if(root.requireMounted)invariant((await volumeStat(base)).mountPoint===base,'USB_DISCONNECTED','Connect a USB drive, then refresh the storage list.');
    return { target, root };
  }
  async function browse({ kind, source = null }) {
    invariant(Object.hasOwn(extensions, kind), 'FILE_KIND', 'Choose packages, payloads or cheats.');
    if (!source) {
      const entries = [];
      for (const root of roots) {
        try { const { target } = await resolve(root.path); if ((await fs.stat(target)).isDirectory()) entries.push({ name: root.label, source: target, directory: true, usb: Boolean(root.requireMounted) }); }
        catch (e) { if (!['ENOENT', 'FILE_LINK','USB_DISCONNECTED'].includes(e.code)) throw e; }
      }
      return { kind, source: null, parent: null, entries, usbMissing:roots.some(r=>r.requireMounted)&&!entries.some(e=>roots.some(r=>r.requireMounted&&path.resolve(r.path)===e.source)) };
    }
    const { target, root } = await resolve(source);
    invariant((await fs.stat(target)).isDirectory(), 'FILE_DIRECTORY', 'This folder is no longer available.');
    const names = await fs.readdir(target, { withFileTypes: true });
    invariant(names.length <= 4096, 'FILE_DIRECTORY_LIMIT', 'This folder contains too many entries. Move the file into a smaller folder.');
    const entries = [];
    for (const entry of names) {
      if (entry.isSymbolicLink() || entry.name.startsWith('.') || (!entry.isDirectory() && !(entry.isFile() && extensions[kind].test(entry.name)))) continue;
      const source = path.join(target, entry.name), stat = await fs.lstat(source);
      if (stat.isSymbolicLink()) continue;
      entries.push({ name: entry.name, source, directory: stat.isDirectory(), bytes: stat.size, modified: stat.mtimeMs });
    }
    entries.sort((a, b) => Number(b.directory) - Number(a.directory) || a.name.localeCompare(b.name));
    return { kind, source: target, parent: target === path.resolve(root.path) ? null : path.dirname(target), entries };
  }
  async function validate(input) {
    invariant(Object.hasOwn(extensions, input?.kind), 'FILE_KIND', 'Choose a supported file.');
    const { target } = await resolve(input.source), stat = await fs.lstat(target), name = path.basename(target);
    invariant(stat.isFile() && !stat.isSymbolicLink() && extensions[input.kind].test(name), 'FILE_TYPE', 'Choose a supported regular file.');
    invariant(stat.size === input.bytes && stat.mtimeMs === input.modified, 'FILE_CHANGED', 'The selected file changed. Choose it again.');
    if(input.kind==='cheat')invariant(runtime.cheats && stat.size<=4*1048576,
      'CHEAT_TARGET','Choose a cheat file up to 4 MiB.');
    return { kind: input.kind, source: target, name, bytes: stat.size, modified: stat.mtimeMs,
      ...(input.kind==='cheat'?{titleId:input.titleId,version:input.version}:{}) };
  }
  runtime.localFiles = { browse, validate };
  runtime.jobs.register('file.import', {
    validate, resources: () => ['local-file-copy'], timeout: 48 * 60 * 60 * 1000, cancellable: true,
    async run(input, context) {
      try {
        await validate(input);
        if(input.kind==='cheat'){
          const entry=await runtime.cheats.packs.import({source:await fs.readFile(input.source,'utf8'),titleId:input.titleId,version:input.version,fallback:runtime.game});
          return {completed:true,id:entry.sha256,message:'Cheat file imported. Choose a cheat to enable it.'};
        }
        const inbox = input.kind === 'package' ? runtime.packageInbox : runtime.inbox;
        invariant(inbox, 'FILE_IMPORT', 'This file type is not supported by this build.');
        const result = input.kind === 'package' ? await inbox.referenceFile(input.source, input.name, context)
          : await inbox.importFile(input.source, input.name, context);
        return { completed: true, id: result.id, sha256: result.sha256, ...(input.kind === 'package' ? { package: result } : {}),
          message: input.kind === 'package' ? 'Package checked. Ready to install directly from the selected file. Keep USB connected until installation finishes.' : 'File copied and checked. Ready to load.' };
      } catch (error) {
        if(error.code==='UPLOAD_CANCELLED')return {cancelled:true,message:'Copy cancelled. The original file was not changed.'};
        error.noSideEffects = true; throw error;
      }
      finally {
        if (runtime.packageInbox) runtime.packageItems = await runtime.packageInbox.list();
        if (runtime.inbox) runtime.inboxItems = await runtime.inbox.list();
        runtime.sequence++;
      }
    },
    async verify(result, input) {
      if(input.kind==='cheat')return runtime.cheats.list().some(entry=>entry.sha256===result.id);
      const inbox = input.kind === 'package' ? runtime.packageInbox : runtime.inbox;
      return (await inbox.status(result.id)).status === 'verified';
    }
  });
}
