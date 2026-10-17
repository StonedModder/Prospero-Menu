// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoMenu overlay manifests (docs/OVERLAYS.md).
//
// Every entry in the menu except ProsperoSettings comes from a folder in
// /data/ProsperoMenu/overlays/<id>/ that holds an overlay.json. A manifest that
// does not validate still produces a (disabled) row that says why, so a typo is
// visible on the console instead of an overlay silently disappearing.

export const OVERLAY_API = 1;
export const OVERLAY_ID = /^[a-z0-9][a-z0-9_-]{0,14}$/;
// ProsperoMenu pages a manifest can point at with "kind": "builtin". Cheats and saved
// games are intentionally not offered.
export const BUILTIN_PAGES = Object.freeze({
  payloads: { command: '$page', args: { page: 1 } },
  games: { command: '$page', args: { page: 2 } },
  dumper: { command: '$dumper', args: {} },
  'elf-updater': { command: '$elfup', args: {} },
  autoload: { command: '$autoload', args: {} },
  transfers: { command: '$page', args: { page: 3 } },
  packages: { command: '$packages', args: {} },
  files: { command: '$file-manager', args: {} },
  'console-settings': { command: '$page', args: { page: 4 } }
});
const KINDS = ['elf', 'menu', 'builtin'];
// What an ELF overlay may ask the menu to do on its behalf (host services).
export const PERMISSIONS = Object.freeze([]);
const FILE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
const clean = (value, max) => String(value ?? '').replace(/[\x00-\x1f\x7f]/g, ' ').trim().slice(0, max);

export class ManifestError extends Error {
  constructor(message) { super(message); this.code = 'OVERLAY_MANIFEST'; }
}
const fail = message => { throw new ManifestError(message); };

// Returns a normalized, frozen overlay description or throws ManifestError.
export function parseManifest(folder, text) {
  if (!OVERLAY_ID.test(folder)) fail('Folder name must be 1-15 characters: a-z, 0-9, "-" or "_".');
  let raw;
  try { raw = JSON.parse(text); } catch (error) { fail('overlay.json is not valid JSON: ' + clean(error.message, 120)); }
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) fail('overlay.json must contain one JSON object.');
  const kind = raw.kind ?? (raw.page ? 'builtin' : raw.menu ? 'menu' : 'elf');
  if (!KINDS.includes(kind)) fail(`"kind" must be one of ${KINDS.join(', ')}.`);
  // Quietly retire old built-in manifests, including user-renamed folders.
  // Leave their files intact; unrelated manifest errors remain visible.
  if (kind === 'builtin' && raw.page === 'plugins')
    throw Object.assign(new ManifestError('This built-in page has been removed.'), { code: 'OVERLAY_RETIRED' });
  const id = raw.id ?? folder;
  if (id !== folder) fail(`"id" (${clean(id, 20)}) must match the folder name (${folder}).`);
  const api = raw.api ?? OVERLAY_API;
  if (api !== OVERLAY_API) fail(`Overlay API ${clean(api, 8)} is not supported; this ProsperoMenu speaks API ${OVERLAY_API}.`);
  const name = clean(raw.name, 48);
  if (!name) fail('"name" is required.');
  const overlay = {
    id, name, kind, api,
    version: clean(raw.version, 16), author: clean(raw.author, 48),
    description: clean(raw.description, 160),
    order: Number.isInteger(raw.order) && raw.order >= 0 && raw.order <= 9999 ? raw.order : 500,
    keepAlive: raw.keepAlive === true, autostart: raw.autostart === true, hidden: raw.hidden === true
  };
  if (kind === 'elf') {
    overlay.permissions = raw.permissions ?? [];
    if (!Array.isArray(overlay.permissions) || overlay.permissions.some(p => !PERMISSIONS.includes(p)))
      fail(PERMISSIONS.length ? `"permissions" may contain: ${PERMISSIONS.join(', ')}.` : '"permissions" is not used by this API version.');
    overlay.permissions = Object.freeze([...overlay.permissions]);
    overlay.elf = raw.elf ?? id + '.ovl';
    if (typeof overlay.elf !== 'string' || !FILE.test(overlay.elf)) fail('"elf" must be a plain file name inside the overlay folder.');
  } else if (kind === 'menu') {
    overlay.menu = raw.menu ?? 'menu.json';
    if (typeof overlay.menu !== 'string' || !FILE.test(overlay.menu)) fail('"menu" must be a plain file name inside the overlay folder.');
    if (overlay.autostart || overlay.keepAlive) fail('"autostart" and "keepAlive" only apply to ELF overlays.');
  } else {
    overlay.page = raw.page;
    if (!Object.hasOwn(BUILTIN_PAGES, overlay.page)) fail(`"page" must be one of ${Object.keys(BUILTIN_PAGES).join(', ')}.`);
    if (overlay.autostart || overlay.keepAlive) fail('"autostart" and "keepAlive" only apply to ELF overlays.');
  }
  return Object.freeze(overlay);
}

// Compare dotted versions numerically ("1.10.0" > "1.9.2").
export function compareVersions(a, b) {
  const pa = String(a || '0').split(/[.-]/).map(n => parseInt(n, 10) || 0), pb = String(b || '0').split(/[.-]/).map(n => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  return 0;
}

export function sortOverlays(list) {
  return [...list].sort((a, b) => (a.order ?? 500) - (b.order ?? 500) || a.name.localeCompare(b.name) || a.id.localeCompare(b.id));
}
