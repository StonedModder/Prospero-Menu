// SPDX-License-Identifier: GPL-3.0-or-later
// Declarative overlays ("kind": "menu"): a menu.json in the overlay folder
// describes pages of rows and what each row does. No compiler needed, so a
// user can add a launcher for their own payloads with a text editor.
//
// {
//   "pages": {
//     "main":  { "title": "Quick Launch", "items": [
//       { "kind": "header", "label": "Payloads" },
//       { "label": "FTP server", "detail": "Start ftpsrv", "action": { "type": "payload", "file": "ftpsrv.elf" } },
//       { "label": "More", "action": { "type": "page", "page": "more" } } ] },
//     "more": { "title": "More", "items": [ ... ] }
//   }
// }
//
// Actions: page (open another page; Circle returns), overlay (open another
// overlay by id), payload (start an ELF that sits in this overlay's folder),
// notify (show a system notification).
import { sanitizeItem } from './protocol.mjs';

const ACTIONS = ['page', 'overlay', 'payload', 'notify'];
const FILE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
const clean = (value, max) => String(value ?? '').replace(/[\x00-\x1f\x7f]/g, ' ').slice(0, max);

export function parseMenu(text) {
  let raw; try { raw = JSON.parse(text); } catch (error) { throw new Error('menu.json is not valid JSON: ' + error.message); }
  if (!raw?.pages || typeof raw.pages !== 'object') throw new Error('menu.json needs a "pages" object.');
  const pages = {};
  for (const [name, page] of Object.entries(raw.pages).slice(0, 64)) {
    if (!Array.isArray(page?.items)) throw new Error(`Page "${name}" needs an "items" array.`);
    pages[name] = { title: clean(page.title, 48), subtitle: clean(page.subtitle, 80), items: page.items.slice(0, 200).map((item, index) => {
      const row = sanitizeItem({ ...item, kind: item.kind === 'header' ? 'header' : 'item' }, index);
      const action = item.action;
      if (action != null) {
        if (!ACTIONS.includes(action.type)) throw new Error(`Page "${name}" item ${index}: unknown action "${clean(action.type, 16)}".`);
        if (action.type === 'page' && !Object.hasOwn(raw.pages, action.page)) throw new Error(`Page "${name}" item ${index}: no page "${clean(action.page, 32)}".`);
        if (action.type === 'payload' && !FILE.test(action.file || '')) throw new Error(`Page "${name}" item ${index}: "file" must be a file in the overlay folder.`);
        row.action = { type: action.type, page: action.page, id: action.id, file: action.file, text: clean(action.text, 120) };
      }
      // Rows without an action stay focusable so their description can be read.
      return row;
    }) };
  }
  if (!pages[raw.start || 'main']) throw new Error(`menu.json has no start page "${clean(raw.start || 'main', 32)}".`);
  return { start: raw.start || 'main', pages };
}

export class MenuOverlay {
  constructor(overlay, menu) { this.overlay = overlay; this.menu = menu; this.stack = [menu.start]; }
  get page() { return this.menu.pages[this.stack.at(-1)]; }
  view() {
    const page = this.page;
    return { title: page.title || this.overlay.name, subtitle: page.subtitle || (this.overlay.version ? 'v' + this.overlay.version : ''),
      rows: page.items.map(item => ({ label: item.label, detail: item.detail, value: item.value, kind: item.kind === 'header' ? 'header' : undefined,
        enabled: item.enabled, command: item.enabled ? '$pm-menu' : null, args: { id: item.id } })) };
  }
  // Returns the action for the deck to perform, or null when handled here.
  select(id) {
    const item = this.page.items.find(x => x.id === id);
    if (!item?.action) return null;
    if (item.action.type === 'page') { this.stack.push(item.action.page); return null; }
    return item.action;
  }
  back() { if (this.stack.length > 1) { this.stack.pop(); return true; } return false; }
}
