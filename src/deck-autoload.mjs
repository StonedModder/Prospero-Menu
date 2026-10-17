// SPDX-License-Identifier: GPL-3.0-or-later
// Controller rows for the startup order: the ps5_autoloader/autoload.txt that
// the jailbreak autoloaders execute at boot. Shared by Autoload config and
// Elf-Updater.
//
// Edits are made in place and written back immediately. Lines the page does not
// understand are kept as they are. A payload moves together with the waits that
// follow it, so its timing travels with it; waits are adjusted with Left/Right.
// Turning a payload off comments its line out ("# name.elf"), which every
// autoloader skips, and turning it on restores it.
import { WAITS } from './autoload.mjs';

export const WAIT_MAX = 600000;
const PAYLOAD = /^[^\s#/\\][^\s/\\]*\.(elf|bin|js)$/i;

export const seconds = ms => ms % 1000 === 0 ? ms / 1000 + ' s' : (ms / 1000).toFixed(ms % 100 === 0 ? 1 : 2) + ' s';
// Left/Right: half a second up to 10 s, then whole seconds, then 5 s steps.
export function stepWait(ms, direction) {
  const up = direction > 0;
  const step = (up ? ms >= 30000 : ms > 30000) ? 5000 : (up ? ms >= 10000 : ms > 10000) ? 1000 : 500;
  const next = up ? Math.floor(ms / step) * step + step : Math.ceil(ms / step) * step - step;
  return Math.max(0, Math.min(WAIT_MAX, next));
}
const isDisabled = entry => entry.kind === 'comment' && PAYLOAD.test(entry.text.trim());
const isPayload = entry => entry.kind === 'payload';

// A "step" is a line with the waits that follow it, so a payload's timing moves
// with it. Waits before the first line are steps of their own.
export function steps(entries) {
  const out = [];
  for (let i = 0; i < entries.length;) {
    let end = i + 1;
    if (entries[i].kind !== 'wait') while (end < entries.length && entries[end].kind === 'wait') end++;
    out.push([i, end]); i = end;
  }
  return out;
}
export function stepAt(entries, index) {
  if (entries[index]?.kind === 'wait') return [index, index + 1];
  return steps(entries).find(([start]) => start === index) || [index, index + 1];
}

const label = (entry, number) => entry.kind === 'wait' ? 'Wait'
  : entry.kind === 'directive' ? '@' + entry.name
  : isDisabled(entry) ? entry.text.trim()
  : entry.kind === 'comment' ? '# ' + entry.text
  : entry.kind === 'unknown' ? entry.text
  : (number ? number + '. ' : '') + entry.name;

function payloadState(entry, view) {
  const name = isDisabled(entry) ? entry.text.trim() : entry.name;
  if (name.startsWith('/')) return { missing: false, name };
  return { missing: Array.isArray(view.files) && !view.files.includes(name), name };
}

function entryDetail(entry, view) {
  if (entry.kind === 'wait') return 'Delay before the next line. Left/Right to change, X for options.';
  if (entry.kind === 'directive') return 'Autoloader directive.';
  if (entry.kind === 'unknown') return 'Line kept as written.';
  if (entry.kind === 'comment' && !isDisabled(entry)) return 'Comment.';
  const { missing, name } = payloadState(entry, view), note = view.notes?.[name];
  const parts = [];
  if (isDisabled(entry)) parts.push('Turned off: the autoloader skips it');
  if (missing) parts.push('File not found in ' + view.dir + ': the autoloader cannot load it');
  if (note) parts.push(note.detail);
  if (!parts.length) parts.push('Loaded from ' + (name.startsWith('/') ? name : view.dir));
  return parts.join(' · ');
}

function entryValue(entry, view) {
  if (entry.kind === 'wait') return '< ' + seconds(entry.ms) + ' >';
  if (!isPayload(entry) && !isDisabled(entry)) return '';
  const { missing, name } = payloadState(entry, view);
  if (isDisabled(entry)) return 'Off';
  if (missing) return 'Missing';
  return view.notes?.[name]?.value || '';
}

export function autoloadRows(view) {
  const rows = [], row = (text, info, command = null, args = {}, extra = {}) =>
    rows.push({ label: text, detail: info, command, args, enabled: Boolean(command), ...extra });

  if (view.kind === 'empty') {
    row('No ps5_autoloader folder found',
      'Create /data/ps5_autoloader/autoload.txt, or plug in a USB drive that has one, then Refresh.');
    row('Refresh', 'Read the folders again.', '$autoload');
    return rows;
  }
  if (view.kind === 'choose') {
    row('Autoloaders found', 'More than one ps5_autoloader exists. A USB drive is read before the console.');
    for (const source of view.sources) row(source.label,
      `${source.dir} · ${source.exists ? source.entries.filter(isPayload).length + ' payloads' : 'no autoload.txt'}`,
      '$autoload-source', { id: source.id });
    return rows;
  }
  if (view.kind === 'add') {
    row('Add a payload', 'Files in ' + view.dir + '. It is added at the end; move it afterwards.');
    const listed = new Set(view.entries.filter(e => isPayload(e) || isDisabled(e)).map(e => payloadState(e, view).name));
    const added = view.files.filter(name => !listed.has(name));
    for (const name of added) row(name, view.notes?.[name]?.detail || 'Add to the startup order.', '$autoload-add-file', { name });
    if (!added.length) row('No payload files to add', 'Every .elf, .bin and .js in ' + view.dir + ' is already listed.');
    row('Back to the startup order', '', '$autoload-list');
    return rows;
  }
  if (view.kind === 'entry') {
    const entry = view.entries[view.index];
    if (!entry) { row('Entry no longer available', 'Return to the list.'); row('Back to the startup order', '', '$autoload-list'); return rows; }
    const [start, end] = stepAt(view.entries, view.index);
    const runnable = isPayload(entry) || isDisabled(entry);
    row(label(entry), entryDetail(entry, view), '$info', {}, { value: entryValue(entry, view) });
    if (entry.kind === 'wait') {
      row('Wait', 'Left/Right changes it. X cycles common values.', '$autoload-wait', {}, { value: '< ' + seconds(entry.ms) + ' >', adjust: true });
    } else if (runnable) {
      const wait = end > start + 1 ? view.entries.slice(start + 1, end).reduce((sum, w) => sum + w.ms, 0) : 0;
      row('Wait after it', wait ? 'Time before the next payload starts. Left/Right changes it.' : 'No wait: the next payload starts straight away. Right adds one.',
        '$autoload-wait', {}, { value: '< ' + seconds(wait) + ' >', adjust: true });
    }
    const first = entry.kind === 'wait' ? view.index === 0 : start === 0, last = entry.kind === 'wait' ? view.index === view.entries.length - 1 : end === view.entries.length;
    if (!first) row('Move up', runnable ? 'Load it before the previous payload. Its wait moves with it.' : 'Move this line up.', '$autoload-move', { delta: -1 });
    if (!last) row('Move down', runnable ? 'Load it after the next payload. Its wait moves with it.' : 'Move this line down.', '$autoload-move', { delta: 1 });
    if (runnable && !first) row('Move to the top', 'Load it first.', '$autoload-move', { to: 'top' });
    if (runnable && !last) row('Move to the bottom', 'Load it last.', '$autoload-move', { to: 'bottom' });
    if (isPayload(entry)) row('Turn off', 'Keep it in the list but skip it at boot (the line becomes a comment).', '$autoload-toggle');
    if (isDisabled(entry)) row('Turn on', 'Load it at boot again.', '$autoload-toggle');
    row(runnable ? 'Remove from startup' : 'Remove', runnable ? 'Delete it and its wait from autoload.txt. The file itself stays.' : 'Delete this line.', '$autoload-remove', {}, { confirm: runnable });
    row('Back to the startup order', '', '$autoload-list');
    return rows;
  }
  // list
  const payloads = view.entries.filter(isPayload);
  const total = view.entries.filter(e => e.kind === 'wait').reduce((sum, w) => sum + w.ms, 0);
  const missing = payloads.filter(e => payloadState(e, view).missing).length;
  row('Startup order', view.dir + (view.exists ? ` · ${payloads.length} payload${payloads.length === 1 ? '' : 's'} · ${seconds(total)} of waits` : ' · autoload.txt will be created on the first change'),
    '$info', {}, { value: missing ? missing + ' missing' : '' });
  if (view.shadowedBy) row('A USB autoloader is read first', `${view.shadowedBy} has its own autoload.txt, so the console uses that one at boot and not this list.`);
  let number = 0;
  view.entries.forEach((entry, index) => {
    if (isPayload(entry)) number++;
    row(label(entry, isPayload(entry) ? number : 0), entryDetail(entry, view), '$autoload-entry', { index },
      { value: entryValue(entry, view), adjust: entry.kind === 'wait' });
  });
  if (!view.entries.length) row('No entries yet', 'Add a payload below, or copy an autoload.txt into this folder.');
  row('Add payload', 'Choose a file that is already in this folder.', '$autoload-add');
  row('Add wait', 'A delay at the end of the list. Move it afterwards.', '$autoload-insert-wait');
  row('Refresh', 'Read autoload.txt again.', '$autoload-refresh');
  return rows;
}

const nextWait = ms => WAITS[(WAITS.indexOf(WAITS.includes(ms) ? ms : 0) + 1) % WAITS.length] || 0;

export async function startAutoload(runtime, { notes = null } = {}) {
  const { sources } = await runtime.execute('autoload.scan', {});
  const present = sources.filter(source => source.exists || source.files.length);
  if (!present.length) return { kind: 'empty', sources };
  if (present.length === 1) return openSource(present[0], sources, notes);
  return { kind: 'choose', sources: present, notes };
}

// The console folder, used by Elf-Updater whatever else is plugged in.
export async function openInternal(runtime, notes = null) {
  const { sources } = await runtime.execute('autoload.scan', {});
  const internal = sources.find(s => s.id === 'internal') || sources[0];
  return openSource(internal, sources, notes);
}

export function openSource(source, all = [], notes = null) {
  // The autoloaders read USB drives before the console.
  const shadow = source.id === 'internal' ? (all || []).find(s => s.id !== 'internal' && s.exists) : null;
  return { kind: 'list', source: source.id, dir: source.dir, file: source.file, label: source.label,
    exists: source.exists, entries: [...source.entries], files: [...source.files], index: 0,
    shadowedBy: shadow ? shadow.label : null, notes: notes || null };
}

export async function refreshSource(runtime, view) {
  const { sources } = await runtime.execute('autoload.scan', {});
  const source = sources.find(s => s.id === view.source);
  if (!source) return view;
  const next = openSource(source, sources, view.notes);
  return { ...next, kind: view.kind === 'add' || view.kind === 'entry' ? view.kind : 'list', index: Math.min(view.index, Math.max(0, next.entries.length - 1)), sources: view.sources };
}

export function findPayload(view, name) {
  return view.entries.findIndex(e => (isPayload(e) && e.name === name) || (isDisabled(e) && e.text.trim() === name));
}

// Apply one edit and write it back. The saved file is re-read so the view always
// shows exactly what the autoloader will read.
export async function autoloadEdit(runtime, view, change) {
  let entries = view.entries.map(e => ({ ...e }));
  let index = change.index ?? view.index;
  const entry = entries[index];
  if (change.kind === 'move') {
    if (entry?.kind === 'wait') {
      const target = change.to === 'top' ? 0 : change.to === 'bottom' ? entries.length - 1 : index + change.delta;
      if (target >= 0 && target < entries.length) { entries.splice(index, 1); entries.splice(target, 0, entry); index = target; }
    } else {
      const all = steps(entries), k = all.findIndex(([start]) => start === index), order = all.map((_, i) => i);
      if (k >= 0) {
        if (change.to === 'top') { order.splice(k, 1); order.unshift(k); }
        else if (change.to === 'bottom') { order.splice(k, 1); order.push(k); }
        else { const j = k + change.delta; if (j >= 0 && j < all.length) [order[k], order[j]] = [order[j], order[k]]; }
        const moved = [];
        for (const i of order) { if (i === k) index = moved.length; moved.push(...entries.slice(all[i][0], all[i][1])); }
        entries = moved;
      }
    }
  } else if (change.kind === 'remove') {
    const [start, end] = isPayload(entry) || isDisabled(entry) ? stepAt(entries, index) : [index, index + 1];
    entries.splice(start, end - start);
    index = Math.max(0, Math.min(start, entries.length - 1));
  } else if (change.kind === 'add-file') {
    entries.push({ kind: 'payload', name: change.name }); index = entries.length - 1;
  } else if (change.kind === 'insert-wait') {
    entries.push({ kind: 'wait', ms: 1000 }); index = entries.length - 1;
  } else if (change.kind === 'toggle') {
    if (isPayload(entry)) entries[index] = { kind: 'comment', text: entry.name };
    else if (isDisabled(entry)) entries[index] = { kind: 'payload', name: entry.text.trim() };
  } else if (change.kind === 'wait' || change.kind === 'wait-step') {
    // On a wait line: change it. On a payload: change the wait that follows it.
    if (entry?.kind === 'wait') {
      entry.ms = change.kind === 'wait' ? nextWait(entry.ms) : stepWait(entry.ms, change.delta);
    } else {
      const [start, end] = stepAt(entries, index);
      const current = entries.slice(start + 1, end).reduce((sum, w) => sum + w.ms, 0);
      const ms = change.kind === 'wait' ? nextWait(current) : stepWait(current, change.delta);
      entries.splice(start + 1, end - start - 1, ...(ms > 0 ? [{ kind: 'wait', ms }] : []));
    }
  }
  const saved = await runtime.execute('autoload.save', { source: view.source, entries });
  view.entries = saved.entries; view.exists = true;
  view.index = Math.max(0, Math.min(index, view.entries.length - 1));
  view.kind = change.kind === 'remove' || change.kind === 'insert-wait' ? 'list' : change.kind === 'add-file' ? 'entry' : view.kind;
  return view;
}
