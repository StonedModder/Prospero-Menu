// SPDX-License-Identifier: GPL-3.0-or-later
// Controller rows for the Elf-Updater overlay (see elf-updater.mjs).
import { findPayload } from './deck-autoload.mjs';
import { invariant } from './errors.mjs';

const ACTIVE = ['queued', 'running', 'cancelling', 'held'];
const KINDS = ['elfupdater.check', 'elfupdater.install', 'elfupdater.update-all'];
const date = ms => ms ? new Date(ms).toISOString().slice(0, 10) : '';
const size = bytes => bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB';
const ago = (ms, now) => { const m = Math.round((now - ms) / 60000); return m < 1 ? 'just now' : m < 60 ? m + ' min ago' : m < 1440 ? Math.round(m / 60) + ' h ago' : Math.round(m / 1440) + ' days ago'; };
const short = hex => hex ? hex.slice(0, 12) + '…' : 'unknown';

export async function openElfUpdater(runtime, view = {}) {
  invariant(runtime.elfUpdater, 'FEATURE_UNAVAILABLE', 'Elf-Updater needs the console.');
  await runtime.elfUpdater.load();
  await runtime.elfUpdater.scanLocal();
  const next = { kind: 'main', ...view, notice: '' };
  try { next.startup = await readStartup(runtime); } catch (error) { next.startup = { error: error.message }; }
  return next;
}

export async function readStartup(runtime) {
  const { sources } = await runtime.execute('autoload.scan', {});
  const internal = sources.find(s => s.id === 'internal');
  return { entries: internal?.entries || [], exists: Boolean(internal?.exists), dir: internal?.dir,
    shadowedBy: sources.find(s => s.id !== 'internal' && s.exists)?.label || null };
}

// Labels for the startup-order page: which source a file belongs to and its state.
export function updaterNotes(runtime) {
  const status = runtime.elfUpdater?.status();
  if (!status) return null;
  const notes = {};
  for (const item of status.items) {
    if (!item.local) continue;
    notes[item.file] = { value: item.short === 'Up to date' ? (item.installed?.tag || 'Current') : item.short, detail: item.name + ': ' + item.summary };
  }
  return notes;
}

function latestJob(jobs) {
  return [...(jobs || [])].reverse().find(job => KINDS.includes(job.kind)) || null;
}

function jobRow(row, job, now) {
  if (!job) return;
  if (ACTIVE.includes(job.status)) {
    const p = job.progress, pct = p?.total ? Math.floor(p.completed * 100 / p.total) + '%' : '';
    row(job.kind === 'elfupdater.check' ? 'Checking GitHub' : 'Installing', p?.phase || 'Waiting to start', 'job.cancel', { id: job.id }, { value: pct });
    return true;
  }
  if (now - (job.updatedAt || 0) > 5 * 60 * 1000) return false;
  if (job.status === 'completed') row(job.kind === 'elfupdater.check' ? 'Check finished' : 'Installed', job.result?.message || 'Done.', '$info', {}, { value: 'Done' });
  else if (job.status === 'failed' || job.status === 'interrupted' || job.status === 'uncertain') row('Needs attention', job.error?.message || job.result?.message || 'The operation did not finish.', null, {}, { value: 'Failed' });
  else if (job.status === 'cancelled') row('Cancelled', job.result?.message || 'Nothing was replaced.', '$info');
  return false;
}

function startupState(view, file) {
  const entries = view.startup?.entries || [];
  const index = findPayload({ entries }, file);
  if (index < 0) return { listed: false };
  const entry = entries[index];
  const position = entries.slice(0, index + 1).filter(e => e.kind === 'payload').length;
  return { listed: true, off: entry.kind === 'comment', position, index };
}

export function elfUpdaterRows(runtime, view, state, now = Date.now()) {
  const rows = [], row = (label, detail, command = null, args = {}, extra = {}) =>
    rows.push({ label, detail, command, args, enabled: Boolean(command), ...extra });
  const header = label => rows.push({ label, kind: 'header', enabled: false });
  const info = (label, detail, extra = {}) => row(label, detail, '$info', {}, extra);
  const status = runtime.elfUpdater.status(), job = latestJob(state.jobs);
  const busy = jobRow(row, job, now);
  const enqueue = (kind, input) => busy ? null : 'job.enqueue';
  const item = view.id ? status.items.find(i => i.id === view.id) : null;

  if (view.kind === 'versions' && item) {
    const releases = item.latest ? (runtime.elfUpdater.releases?.(item.id) || []) : [];
    info(item.name + ' releases', `The last ${releases.length} releases with ${item.file}. Installing one replaces your copy; the old one is kept as ${item.file}.prev.`);
    for (const release of releases) {
      const installed = item.installed?.tag === release.tag, latest = item.latest?.tag === release.tag;
      row(release.tag, `${date(release.publishedAt)} · ${size(release.asset.size)} · SHA-256 ${short(release.asset.sha256)}${release.prerelease ? ' · pre-release' : ''}`,
        installed ? null : enqueue(), { kind: 'elfupdater.install', input: { id: item.id, tag: release.tag } },
        { value: installed ? 'Installed' : latest ? 'Latest' : release.prerelease ? 'Pre' : '', confirm: true });
    }
    if (!releases.length) row('No releases listed', 'Check for updates first.');
    row('Back', '', '$elfup-back');
    return rows;
  }

  if (view.kind === 'source' && item) {
    info(item.name, item.about || item.repo, { value: item.short });
    info('Installed', item.local
      ? `${item.file} · ${size(item.local.size)} · SHA-256 ${short(item.local.sha256)}`
      : item.foundAs.length ? `Not as ${item.file}, but ${item.foundAs[0].name} is ${item.foundAs[0].release.tag}.` : `No ${item.file} in ${status.folder}.`,
      { value: item.installed ? item.installed.tag : item.local ? (item.status === 'unchecked' ? '?' : 'Unknown') : 'None' });
    info('Status', item.summary);
    if (item.latest) info('Latest', `On GitHub since ${date(item.latest.publishedAt)} · ${size(item.latest.asset.size)} · ` +
      (item.latest.asset.sha256 ? 'SHA-256 ' + short(item.latest.asset.sha256) : 'GitHub published no checksum for this file'), { value: item.latest.tag });
    else if (item.error) row('GitHub', item.error, null, {}, { value: 'Error' });
    else info('Latest', item.checkedAt ? 'No release on GitHub has a matching file.' : 'Not checked on GitHub yet.');
    if (item.latest) {
      const verb = item.status === 'missing' ? 'Install' : item.status === 'current' ? 'Reinstall' : item.status === 'outdated' ? 'Update to' : 'Replace with';
      const detail = item.status === 'missing' ? `Download, verify and save it as ${item.file}. It loads at boot once it is in the startup order.`
        : `Download and verify it, then replace ${item.file}. Your current copy is kept as ${item.file}.prev.`;
      row(verb + ' ' + item.latest.tag, detail, enqueue(), { kind: 'elfupdater.install', input: { id: item.id } }, { confirm: item.status !== 'missing' });
    }
    const startup = startupState(view, item.file);
    if (item.local) {
      if (!startup.listed) row('Add to startup order', `Not in autoload.txt yet, so it does not load at boot. Adds it at the end, then you can move it.`, '$elfup-startup', { id: item.id });
      else row(startup.off ? 'Turned off in startup order' : 'In startup order', startup.off ? 'Listed but turned off. Open it to turn it on or move it.' : 'Open it to move it or change its wait.',
        '$elfup-startup', { id: item.id }, { value: startup.off ? 'Off' : '#' + startup.position });
    }
    if (item.checkedAt) row('Other versions', 'Install an older release or a pre-release.', '$elfup-versions', { id: item.id });
    row('Check this payload', item.checkedAt ? 'Last checked ' + ago(item.checkedAt, now) + '.' : 'Read its releases from GitHub.', enqueue(), { kind: 'elfupdater.check', input: { id: item.id } });
    row('Back', '', '$elfup-back');
    return rows;
  }

  header('Payloads in ' + status.folder);
  for (const entry of status.items) {
    const startup = startupState(view, entry.file);
    const where = entry.local ? (startup.listed ? (startup.off ? 'turned off at boot' : 'loads at boot #' + startup.position) : 'not in the startup order') : '';
    row(entry.name, [entry.summary, where].filter(Boolean).join(' · '), '$elfup-source', { id: entry.id }, { value: entry.short });
  }
  const checked = status.items.map(i => i.checkedAt).filter(Boolean);
  row('Check for updates', checked.length ? 'Last checked ' + ago(Math.min(...checked), now) + '. Compares your files with GitHub\'s SHA-256.' : 'Read the latest releases from GitHub and compare SHA-256.',
    enqueue(), { kind: 'elfupdater.check', input: {} });
  const updatable = status.items.filter(i => i.latest && (i.status === 'outdated' || (i.status === 'unknown' && i.local)));
  if (updatable.length) row(`Update ${updatable.length === 1 ? updatable[0].name : 'all ' + updatable.length}`, updatable.map(i => `${i.name} → ${i.latest.tag}`).join(', ') + '. Old copies are kept as .prev.',
    enqueue(), { kind: 'elfupdater.update-all', input: { ids: updatable.map(i => i.id) } }, { confirm: true });
  header('Startup');
  if (view.startup?.shadowedBy) row('A USB autoloader is read first', `${view.startup.shadowedBy} has its own autoload.txt, so the console list below is not used at boot.`);
  const listed = (view.startup?.entries || []).filter(e => e.kind === 'payload').length;
  row('Startup order', `Reorder, time, turn off or add the payloads in autoload.txt (${listed} at boot).`, '$elfup-autoload');
  header('Settings');
  row('Include pre-releases', 'Offer pre-releases as the latest version.', '$elfup-prerelease', {}, { on: status.includePrereleases });
  info('Add your own payloads', `List more GitHub repositories in ${status.configFile}, then Refresh.`);
  for (const error of status.errors) row('Source not loaded', error, null, {}, { value: 'Error' });
  row('Refresh', 'Read the folder and settings again.', '$elfup-refresh');
  return rows;
}
