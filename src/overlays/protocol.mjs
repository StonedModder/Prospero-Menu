// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay wire protocol, API 1: one JSON object per line over loopback TCP.
// Everything an overlay sends is validated and clamped here before it can
// reach a deck frame; ShellUI only ever sees what this module returns.

export const MAX_LINE = 256 * 1024;
export const MAX_ITEMS = 500;
const ITEM_KINDS = ['header', 'item', 'toggle', 'slider', 'text'];
const TOKEN = /^[A-Za-z0-9_.:-]{1,48}$/;
const clean = (value, max) => String(value ?? '').replace(/[\x00-\x1f\x7f]/g, ' ').slice(0, max);
const int = (value, fallback) => Number.isFinite(value) ? Math.trunc(Math.max(-1e9, Math.min(1e9, value))) : fallback;

export class ProtocolError extends Error {
  constructor(message) { super(message); this.code = 'OVERLAY_PROTOCOL'; }
}
const fail = message => { throw new ProtocolError(message); };

export function sanitizeItem(raw, index) {
  if (!raw || typeof raw !== 'object') fail(`Item ${index} is not an object.`);
  const kind = raw.kind ?? 'item';
  if (!ITEM_KINDS.includes(kind)) fail(`Item ${index} has unknown kind "${clean(kind, 16)}".`);
  const id = raw.id == null ? `item-${index}` : String(raw.id);
  if (!TOKEN.test(id)) fail(`Item ${index} id must be 1-48 characters: letters, digits, _ . : -`);
  const item = { kind, id, label: clean(raw.label, 95), detail: clean(raw.detail, 159), enabled: raw.enabled !== false && kind !== 'header' };
  if (kind === 'toggle') item.on = raw.on === true;
  if (kind === 'slider') {
    item.min = int(raw.min, 0); item.max = int(raw.max, 100);
    if (item.max < item.min) fail(`Slider ${id}: max is below min.`);
    item.step = Math.max(1, int(raw.step, 1)); item.current = Math.min(item.max, Math.max(item.min, int(raw.current ?? raw.value, item.min)));
    item.unit = clean(raw.unit, 8);
  } else if (raw.value != null) item.value = clean(raw.value, 31);
  return item;
}

export function sanitizePage(raw) {
  if (!Array.isArray(raw.items)) fail('A page needs an "items" array.');
  if (raw.items.length > MAX_ITEMS) fail(`A page holds at most ${MAX_ITEMS} items.`);
  const items = raw.items.map(sanitizeItem), seen = new Set();
  for (const item of items) { if (seen.has(item.id)) fail(`Duplicate item id "${item.id}".`); seen.add(item.id); }
  return { title: clean(raw.title, 48), subtitle: clean(raw.subtitle, 80), footer: raw.footer == null ? null : clean(raw.footer, 80), items };
}

// Apply a patch {set:{id:{label,value,detail,on,current,enabled}}} in place.
export function applyPatch(page, raw) {
  if (!page) fail('Patch before the first page.');
  if (!raw.set || typeof raw.set !== 'object') fail('A patch needs a "set" object.');
  const keys = Object.keys(raw.set);
  if (keys.length > MAX_ITEMS) fail('Patch is too large.');
  for (const id of keys) {
    const item = page.items.find(x => x.id === id), change = raw.set[id];
    if (!item || !change || typeof change !== 'object') continue; // stale ids are harmless
    if ('label' in change) item.label = clean(change.label, 95);
    if ('detail' in change) item.detail = clean(change.detail, 159);
    if ('enabled' in change && item.kind !== 'header') item.enabled = change.enabled !== false;
    if ('on' in change && item.kind === 'toggle') item.on = change.on === true;
    if ('current' in change && item.kind === 'slider') item.current = Math.min(item.max, Math.max(item.min, int(change.current, item.current)));
    if ('value' in change && item.kind !== 'slider') item.value = change.value == null ? undefined : clean(change.value, 31);
  }
  if (raw.title != null) page.title = clean(raw.title, 48);
  if (raw.subtitle != null) page.subtitle = clean(raw.subtitle, 80);
  return page;
}

// Deck rows for a sanitized page. Each row's command routes back to the host.
export function pageRows(page) {
  return page.items.map(item => {
    const row = { label: item.label, detail: item.detail, enabled: item.enabled, kind: item.kind === 'header' ? 'header' : undefined,
      command: item.enabled ? '$pm-overlay' : null, args: { id: item.id } };
    if (item.kind === 'toggle') { row.value = item.on ? 'On' : 'Off'; row.on = item.on; }
    else if (item.kind === 'slider') { row.value = `< ${item.current}${item.unit} >`; row.adjust = true; }
    else if (item.value != null) row.value = item.value;
    return row;
  });
}

// Line framing: feed raw chunks, get complete JSON messages back.
export class LineReader {
  constructor() { this.buffer = ''; }
  push(chunk) {
    this.buffer += chunk.toString('utf8');
    if (this.buffer.length > MAX_LINE && !this.buffer.includes('\n')) fail('Message line is too long.');
    const lines = this.buffer.split('\n'); this.buffer = lines.pop();
    const messages = [];
    for (const line of lines) {
      const text = line.trim(); if (!text) continue;
      if (text.length > MAX_LINE) fail('Message line is too long.');
      let message; try { message = JSON.parse(text); } catch { fail('Message is not valid JSON.'); }
      if (!message || typeof message !== 'object' || typeof message.type !== 'string') fail('Each message needs a "type".');
      messages.push(message);
    }
    return messages;
  }
}
