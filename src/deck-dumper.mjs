// SPDX-License-Identifier: GPL-3.0-or-later
// Controller rows for the App Dumper overlay: drive the ps5-app-dumper payload
// from the console menu and show its live progress.
import { progressText, rateText } from './app-dumper.mjs';

const active = job => ['starting', 'running', 'copying', 'decrypting'].includes(String(job?.state)) || (job?.state && job.state !== 'idle' && job.state !== 'done' && job.state !== 'failed' && job.state !== 'aborted');

export async function loadDumper(runtime, view = {}) {
  const status = await runtime.dumper.status();
  const next = { ...view, status, kind: view.kind || 'main', notice: '' };
  if (!status.running) return next;
  try {
    const devices = (await runtime.dumper.devices()) || {};
    next.devices = devices;
    const targets = (devices.targets || []).filter(t => t.writable);
    next.targets = targets;
    if (!next.target && targets.length === 1) next.target = targets[0];
    if (next.target && !targets.some(t => t.mount === next.target.mount)) next.target = null;
    if (!next.title) {
      const titles = (devices.apps || []).filter(a => a.dir && a.installed >= 0);
      const running = titles.find(a => a.titleId && String(status.job?.titleId || '') === a.titleId);
      next.titles = titles; next.title = running || null;
    }
  } catch (error) { next.notice = error.message || 'The App Dumper did not answer.'; }
  return next;
}

export function dumperRows(view) {
  const rows = [], row = (label, detail, command = null, args = {}, extra = {}) =>
    rows.push({ label, detail, command, args, enabled: Boolean(command), ...extra });
  const status = view.status || {};
  if (!status.running) {
    row('App Dumper is not running', 'Load ps5-app-dumper.elf from Payloads, then Refresh. The payload serves its interface on 127.0.0.1:8081.');
    row('Refresh', 'Look for the payload again.', '$dumper-refresh');
    return rows;
  }
  const job = status.job || {};
  if (active(job)) {
    const bar = progressText(job.copiedBytes, job.totalBytes);
    row('Dumping ' + (job.title || job.titleId || ''), job.stage || job.currentFile || job.message || 'Working', null, {}, { value: bar });
    const rate = rateText(job);
    if (rate) row('Transfer', rate);
    row('Stop dump', 'The files written so far stay behind incomplete.', '$dumper-stop', {}, { confirm: true });
    row('Refresh', 'Read the progress again.', '$dumper-refresh');
    return rows;
  }
  row('App Dumper', 'Running on ' + (status.base || 'the console'), null);
  row('Destination', view.target ? view.target.mount + ' · ' + view.target.freeBytes / 1024 ** 3 + ' GB free' : 'Choose a drive', '$dumper-targets');
  row('Title', view.title ? (view.title.title || view.title.titleId) : 'Choose a title', '$dumper-titles');
  const ready = Boolean(view.title?.dir && view.target?.mount);
  row('Start dump', ready ? 'Copy this title to the drive with live progress.'
    : 'Choose a title on the console and a destination drive.', ready ? '$dumper-start' : null);
  if (job.state && job.state !== 'idle') row('Last result', (job.title || '') + ' · ' + job.state + (job.message ? ' · ' + job.message : ''));
  row('Refresh', 'Read the payload state again.', '$dumper-refresh');
  if (view.kind === 'titles') {
    rows.length = 0;
    row('Choose a title', 'Titles the console has mounted.');
    for (const app of view.titles || []) row(app.title || app.titleId, [app.titleId, app.media === 'disc' ? 'DISC' : 'PKG', app.installing ? 'installing' : app.installed >= 0 ? app.installed + '%' : ''].filter(Boolean).join(' · '), '$dumper-pick', { titleId: app.titleId });
    if (!(view.titles || []).length) row('No titles to dump', 'Start a game first; the dumper copies what the console has mounted.');
    row('Back', '', '$dumper-back');
  } else if (view.kind === 'targets') {
    rows.length = 0;
    row('Choose a destination', 'Only writable drives are listed.');
    for (const target of view.targets || []) row(target.mount, Math.round(target.freeBytes / 1024 ** 3) + ' GB free · ' + target.fs, '$dumper-target', { mount: target.mount });
    if (!(view.targets || []).length) row('No writable drive', 'Plug a USB drive into the console and Refresh.');
    row('Back', '', '$dumper-back');
  }
  return rows;
}

export async function dumperStart(runtime, view) {
  const result = await runtime.dumper.start({ app: view.title.dir, target: view.target.mount });
  return { ...view, notice: result?.started ? 'Dump started' : '', kind: 'main' };
}
export async function dumperStop(runtime, view) {
  await runtime.dumper.abort();
  return { ...view, notice: 'Stopping the dump', kind: 'main' };
}
