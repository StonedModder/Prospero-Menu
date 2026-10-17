// SPDX-License-Identifier: GPL-3.0-or-later
// ps5_autoloader configuration (/data/ps5_autoloader/autoload.txt and the same
// folder on each USB drive). The format is the one the autoloaders read:
//   # comment
//   @sync                     directive
//   payload.elf               loaded from this folder (or an absolute path)
//   !1000                     wait 1000 ms before the next entry
// Order is the load order, so the menu reorders, adds, removes and re-times it.
import { invariant } from './errors.mjs';
import path from 'node:path';

export const FOLDER = 'ps5_autoloader';
export const FILE = 'autoload.txt';
export const MAX_ENTRIES = 256;
export const MAX_BYTES = 64 * 1024;
export const WAITS = [0, 500, 1000, 2000, 5000, 10000];

const clean = (value, max) => String(value ?? '').replace(/[\x00-\x1f\x7f]/g, ' ').trim().slice(0, max);

// A payload entry is a plain file name inside the autoloader folder or an
// absolute path. Nothing else is accepted when saving.
export function payloadName(value) {
  const name = clean(value, 180);
  invariant(name && !/[\r\n]/.test(name) && !name.includes('\0'), 'AUTOLOAD_NAME', 'Invalid payload name.');
  if (name.startsWith('/')) return name;
  invariant(!name.includes('/') && !name.includes('\\') && name !== '.' && name !== '..', 'AUTOLOAD_NAME',
    'Use a file in the autoloader folder or an absolute path.');
  return name;
}

export function parseAutoload(text) {
  const entries = [];
  for (const raw of String(text ?? '').split(/\r\n?|\n/)) {
    const line = raw.trim();
    if (!line) continue;
    if (line.startsWith('#')) { entries.push({ kind: 'comment', text: line.replace(/^#\s?/, '').slice(0, 120) }); continue; }
    if (line.startsWith('!')) {
      const ms = Number.parseInt(line.slice(1), 10);
      entries.push(Number.isFinite(ms) && ms >= 0 && ms <= 600000 ? { kind: 'wait', ms } : { kind: 'unknown', text: line.slice(0, 120) });
      continue;
    }
    if (line.startsWith('@')) { entries.push({ kind: 'directive', name: line.slice(1).trim().slice(0, 32) }); continue; }
    entries.push({ kind: 'payload', name: line.slice(0, 180) });
  }
  return entries;
}

export function serializeAutoload(entries) {
  const lines = entries.map(entry => {
    if (entry.kind === 'comment') return '# ' + clean(entry.text, 120);
    if (entry.kind === 'wait') return '!' + Math.max(0, Math.min(600000, Number(entry.ms) || 0));
    if (entry.kind === 'directive') return '@' + clean(entry.name, 32);
    if (entry.kind === 'unknown') return clean(entry.text, 120);
    return payloadName(entry.name);
  });
  return lines.join('\n') + '\n';
}

export function validateEntries(entries) {
  invariant(Array.isArray(entries) && entries.length <= MAX_ENTRIES, 'AUTOLOAD_LIMIT', 'Too many autoload entries.');
  for (const entry of entries) {
    invariant(entry && typeof entry.kind === 'string', 'AUTOLOAD_ENTRY', 'Invalid autoload entry.');
    if (entry.kind === 'payload') payloadName(entry.name);
    else if (entry.kind === 'wait') invariant(Number.isFinite(entry.ms) && entry.ms >= 0 && entry.ms <= 600000, 'AUTOLOAD_WAIT', 'Invalid wait.');
    else invariant(['comment', 'directive', 'unknown'].includes(entry.kind), 'AUTOLOAD_ENTRY', 'Invalid autoload entry.');
  }
  const text = serializeAutoload(entries);
  invariant(Buffer.byteLength(text) <= MAX_BYTES, 'AUTOLOAD_SIZE', 'The autoload file is too large.');
  return text;
}

// Named roots: the internal drive first, then every USB mount that exists.
export const SOURCES = Object.freeze([
  { id: 'internal', label: 'Console', root: '/data' },
  ...[0, 1, 2, 3, 4, 5, 6, 7].map(n => ({ id: 'usb' + (n + 1), label: 'USB ' + (n + 1), root: '/mnt/usb' + n }))
]);

export function installAutoload(runtime, { fs, sources = SOURCES } = {}) {
  const folder = source => source.root + '/' + FOLDER;
  async function readJson() { /* kept for symmetry with other services */ }
  async function scan() {
    const found = [];
    for (const source of sources) {
      const dir = folder(source), file = dir + '/' + FILE;
      let text = null, names = [];
      try { text = await fs.readFile(file, 'utf8'); } catch { text = null; }
      try {
        names = (await fs.readdir(dir)).filter(name => !name.startsWith('.')).slice(0, 512).map(name => String(name));
      } catch { names = []; }
      const exists = text !== null;
      found.push({ id: source.id, label: source.label, dir, file, exists,
        entries: exists ? parseAutoload(text) : [], files: names.filter(name => /\.(elf|bin|js)$/i.test(name)).sort() });
    }
    return { sources: found };
  }
  async function save(input) {
    const source = sources.find(s => s.id === input?.source);
    invariant(source, 'AUTOLOAD_SOURCE', 'Choose which autoloader to edit.');
    const text = validateEntries(input.entries);
    const dir = folder(source);
    await fs.mkdir(dir, { recursive: true });
    // Write through a temporary name so an autoloader never reads half a file.
    const target = dir + '/' + FILE;
    await fs.writeFile(target + '.tmp', text);
    await fs.rename(target + '.tmp', target);
    runtime.sequence++;
    return { source: source.id, file: target, entries: parseAutoload(text) };
  }
  runtime.autoload = { scan, save };
  return runtime.autoload;
}
