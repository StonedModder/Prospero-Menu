// SPDX-License-Identifier: GPL-3.0-or-later
// Elf-Updater: keeps the payloads in /data/ps5_autoloader up to date from their
// GitHub releases.
//
// GitHub publishes a SHA-256 digest for every release asset. Elf-Updater reads
// the recent releases of each source, hashes the local copy and compares:
//   current     the local file is the newest release
//   outdated    the local file is an older release (named)
//   prerelease  the local file is a pre-release newer than the newest release
//   unknown     the local file matches no release: damaged, modified or custom
//   missing     no local file
// Installing downloads the asset, refuses it unless its SHA-256 matches the
// digest GitHub published, then replaces the file atomically and keeps the
// previous copy as <file>.prev. Installing never runs a payload, and it never
// rewrites autoload.txt: adding a payload to the startup list is a separate,
// explicit step on the Autoload page.
import { createHash } from 'node:crypto';
import { invariant, PmError } from './errors.mjs';

export const FOLDER = '/data/ps5_autoloader';
export const CONFIG_FILE = '/data/ProsperoMenu/elf-updater.json';
export const STATE_FILE = '/data/ProsperoMenu/elf-updater-state.json';
export const HISTORY = 20;               // releases read per source
const API_LIMIT = 6 * 1048576;           // releases JSON
const ASSET_LIMIT = 96 * 1048576;        // one payload
const ID = /^[a-z0-9][a-z0-9_-]{0,31}$/;
const REPO = /^[A-Za-z0-9_.-]{1,100}\/[A-Za-z0-9_.-]{1,100}$/;
const FILE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.(elf|bin)$/i;
const HEX = /^[a-f0-9]{64}$/;

// The payloads every ProsperoMenu offers. A source in elf-updater.json with the
// same id replaces one of these; "hidden" removes it from the page.
export const DEFAULT_SOURCES = Object.freeze([
  { id: 'kstuff-lite', name: 'kstuff-lite', repo: 'EchoStretch/kstuff-lite', asset: '^kstuff\\.elf$', file: 'kstuff.elf',
    about: 'Kernel patches for homebrew. Load it first.' },
  { id: 'shadowmountplus', name: 'ShadowMountPlus', repo: 'drakmor/ShadowMountPlus', asset: '^shadowmountplus\\.elf$', file: 'shadowmountplus.elf',
    about: 'Mounts game images and folders. Load it after kstuff.' },
  { id: 'zftpd', name: 'zftpd', repo: 'seregonwar/zftpd', asset: '^zftpd-ps5-v?[0-9][0-9A-Za-z._-]*\\.elf$', file: 'zftpd-ps5.elf',
    about: 'FTP server for the whole console.' }
].map(Object.freeze));

const clean = (value, max) => String(value ?? '').replace(/[\x00-\x1f\x7f]/g, ' ').trim().slice(0, max);

// "owner/repo", or any github.com URL of the repository (chronicLoader used the
// releases page URL).
export function normalizeRepo(value) {
  let repo = clean(value, 300).replace(/\.git$/i, '');
  const url = /^https?:\/\/(?:www\.)?github\.com\/([^/?#]+)\/([^/?#]+)/i.exec(repo);
  if (url) repo = url[1] + '/' + url[2];
  invariant(REPO.test(repo), 'ELFUP_SOURCE', 'Use a GitHub repository as owner/name.');
  return repo;
}

export function compileRule(rule) {
  const text = clean(rule, 200);
  invariant(text, 'ELFUP_SOURCE', 'Give the source an asset pattern.');
  try { return new RegExp(text.startsWith('re:') ? text.slice(3) : text, 'i'); }
  catch (error) { throw new PmError('ELFUP_SOURCE', 'The asset pattern is not a valid regular expression: ' + clean(error.message, 80)); }
}

export function validateSource(raw) {
  invariant(raw && typeof raw === 'object', 'ELFUP_SOURCE', 'Each source must be an object.');
  const id = clean(raw.id, 32).toLowerCase();
  invariant(ID.test(id), 'ELFUP_SOURCE', 'Source ids use a-z, 0-9, "-" and "_" (up to 32 characters).');
  const repo = normalizeRepo(raw.repo ?? raw.release_url);
  const asset = clean(raw.asset ?? raw.asset_rule, 200);
  compileRule(asset);
  const file = clean(raw.file ?? raw.autoload_preview_name, 64);
  invariant(FILE.test(file), 'ELFUP_SOURCE', `"${file || '(empty)'}" is not a valid file name. Use a plain name ending in .elf or .bin.`);
  return Object.freeze({ id, repo, asset, file, name: clean(raw.name, 40) || repo.split('/')[1], about: clean(raw.about, 120) });
}

// Defaults first, in their order, then the user's own sources.
export function mergeSources(config = {}) {
  const hidden = new Set(Array.isArray(config.hidden) ? config.hidden.map(String) : []);
  const sources = DEFAULT_SOURCES.map(source => ({ ...source })), errors = [];
  for (const raw of Array.isArray(config.sources) ? config.sources.slice(0, 32) : []) {
    try {
      const source = validateSource(raw), at = sources.findIndex(s => s.id === source.id);
      if (at >= 0) sources[at] = { ...sources[at], ...source, about: source.about || sources[at].about }; else sources.push({ ...source });
    } catch (error) { errors.push(clean(raw?.id || raw?.repo || 'source', 32) + ': ' + error.message); }
  }
  const seen = new Set(), out = [];
  for (const source of sources) {
    if (hidden.has(source.id)) continue;
    if (seen.has(source.file.toLowerCase())) { errors.push(`${source.id}: another source already writes ${source.file}.`); continue; }
    seen.add(source.file.toLowerCase()); out.push(Object.freeze(source));
  }
  return { sources: out, errors };
}

export function normalizeDigest(value) {
  const match = /^sha256:([a-f0-9]{64})$/i.exec(String(value ?? ''));
  return match ? match[1].toLowerCase() : null;
}

// GitHub /releases JSON -> the releases that carry this source's asset, newest
// first. Draft releases are never offered. The download link must be the
// repository's own release download.
export function parseReleases(json, source) {
  invariant(Array.isArray(json), 'ELFUP_RESPONSE', json?.message ? 'GitHub: ' + clean(json.message, 140) : 'GitHub returned an unexpected answer.');
  const rule = compileRule(source.asset), prefix = `https://github.com/${source.repo}/releases/download/`.toLowerCase();
  const releases = [];
  for (const release of json) {
    if (!release || release.draft || typeof release.tag_name !== 'string') continue;
    const asset = (Array.isArray(release.assets) ? release.assets : []).find(a => a && typeof a.name === 'string' && rule.test(a.name));
    if (!asset || typeof asset.browser_download_url !== 'string' || !asset.browser_download_url.toLowerCase().startsWith(prefix)) continue;
    if (!Number.isSafeInteger(asset.size) || asset.size <= 0 || asset.size > ASSET_LIMIT) continue;
    releases.push({
      tag: clean(release.tag_name, 64), name: clean(release.name, 80) || clean(release.tag_name, 64),
      prerelease: Boolean(release.prerelease), publishedAt: Date.parse(release.published_at || release.created_at) || 0,
      asset: { name: clean(asset.name, 120), size: asset.size, sha256: normalizeDigest(asset.digest), url: asset.browser_download_url }
    });
  }
  // The API lists by creation date; a fix re-published later must still win.
  releases.sort((a, b) => b.publishedAt - a.publishedAt);
  return releases.slice(0, HISTORY);
}

export function latestRelease(releases, includePrereleases) {
  return (releases || []).find(release => includePrereleases || !release.prerelease) || null;
}

const sizeText = bytes => bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB';
const tagText = release => release ? release.tag + (release.prerelease ? ' (pre-release)' : '') : '';

// Everything the page shows for one source, from the cached release list and
// the local file's hash. Pure, so it is easy to test.
export function classify(source, { check = null, local = null, others = [], install = null, includePrereleases = false } = {}) {
  const releases = check?.releases || [], latest = latestRelease(releases, includePrereleases);
  const out = { id: source.id, name: source.name, file: source.file, repo: source.repo, latest, local, checkedAt: check?.checkedAt || 0, error: check?.error || null };
  const match = local?.sha256 ? releases.find(r => r.asset.sha256 === local.sha256) : null;
  out.installed = match || null;
  out.foundAs = local ? [] : others.filter(o => releases.some(r => r.asset.sha256 === o.sha256)).map(o => ({ ...o, release: releases.find(r => r.asset.sha256 === o.sha256) }));
  if (!local) {
    out.status = 'missing';
    out.summary = out.foundAs.length ? `Found as ${out.foundAs[0].name} (${out.foundAs[0].release.tag})` : 'Not installed';
    out.short = out.foundAs.length ? 'Other name' : 'Not installed';
    return out;
  }
  if (local.corrupt) { out.status = 'unknown'; out.short = 'Damaged'; out.summary = local.corrupt; return out; }
  if (!releases.length) {
    out.status = check?.checkedAt ? 'unknown' : 'unchecked';
    out.short = check?.checkedAt ? 'No releases' : 'Not checked';
    out.summary = check?.checkedAt ? 'No release of this source has a matching file.' : 'Installed. Check for updates to compare it with GitHub.';
    return out;
  }
  if (match && latest && match.asset.sha256 === latest.asset.sha256) { out.status = 'current'; out.short = 'Up to date'; out.summary = tagText(match) + ' · up to date'; return out; }
  if (match && latest && match.publishedAt > latest.publishedAt) { out.status = 'prerelease'; out.short = 'Pre-release'; out.summary = tagText(match) + ' · newer than ' + latest.tag; return out; }
  if (match) { out.status = 'outdated'; out.short = 'Update'; out.summary = `${tagText(match)} installed · ${tagText(latest)} available`; return out; }
  out.status = 'unknown';
  if (install?.sha256 && install.sha256 !== local.sha256) { out.short = 'Changed'; out.summary = `Changed since Elf-Updater installed ${install.tag}. It may be damaged.`; }
  else if (latest && local.size === latest.asset.size) { out.short = 'Damaged'; out.summary = `Same size as ${latest.tag} but a different SHA-256: the file is damaged.`; }
  else { out.short = 'Unknown'; out.summary = `Matches none of the last ${releases.length} releases: a custom, very old or damaged build.`; }
  return out;
}

export function installElfUpdater(runtime, { fs, folder = FOLDER, configFile = CONFIG_FILE, stateFile = STATE_FILE, now = Date.now, pause = () => new Promise(r => setTimeout(r, 0)) } = {}) {
  let config = { includePrereleases: false, sources: [], hidden: [] }, state = { checks: {}, installs: {}, hashes: {} };
  let sources = mergeSources(config), loaded = false, scan = { at: 0, files: [] };

  const readJson = async (file, fallback) => { try { return JSON.parse(await fs.readFile(file, 'utf8')); } catch (error) { return error.code === 'ENOENT' ? fallback : { $error: error.message }; } };
  async function writeJson(file, value) { await fs.writeFile(file + '.tmp', JSON.stringify(value, null, 1) + '\n'); await fs.rename(file + '.tmp', file); }

  async function load() {
    const raw = await readJson(configFile, null);
    if (raw === null) {
      // First use: write a config users can extend over FTP.
      config = { includePrereleases: false, hidden: [], sources: [] };
      try { await writeJson(configFile, { ...config, $help: 'Add GitHub sources: {"id": "my-tool", "name": "My Tool", "repo": "owner/repo", "asset": "^my-tool.*\\\\.elf$", "file": "my-tool.elf"}. "hidden": ["zftpd"] removes a default.' }); } catch { /* read-only is fine */ }
    } else if (raw.$error) config = { includePrereleases: false, sources: [], hidden: [], $error: raw.$error };
    else config = { includePrereleases: Boolean(raw.includePrereleases), sources: Array.isArray(raw.sources) ? raw.sources : [], hidden: Array.isArray(raw.hidden) ? raw.hidden : [] };
    sources = mergeSources(config);
    if (config.$error) sources.errors.unshift('elf-updater.json could not be read: ' + clean(config.$error, 100));
    const saved = await readJson(stateFile, {});
    state = { checks: saved.checks && typeof saved.checks === 'object' ? saved.checks : {}, installs: saved.installs && typeof saved.installs === 'object' ? saved.installs : {},
      hashes: saved.hashes && typeof saved.hashes === 'object' ? saved.hashes : {} };
    loaded = true;
  }
  const ensure = async () => { if (!loaded) await load(); };
  const saveState = () => writeJson(stateFile, state);

  // SHA-256 of a file, plus a quick sanity check that an .elf is an ELF.
  async function hashFile(file, signal) {
    const handle = await fs.open(file, 'r'), hash = createHash('sha256'), buffer = Buffer.alloc(1048576);
    let at = 0, head = null;
    try {
      for (;;) {
        invariant(!signal?.aborted, 'DOWNLOAD_CANCELLED', 'Cancelled.');
        const { bytesRead } = await handle.read(buffer, 0, buffer.length, at);
        if (!bytesRead) break;
        if (!head) head = Buffer.from(buffer.subarray(0, Math.min(4, bytesRead)));
        hash.update(buffer.subarray(0, bytesRead)); at += bytesRead; await pause();
      }
    } finally { await handle.close(); }
    return { sha256: hash.digest('hex'), size: at, elf: Boolean(head && head.length === 4 && head[0] === 0x7f && head[1] === 0x45 && head[2] === 0x4c && head[3] === 0x46) };
  }

  // Hash the files that matter: every source's own file, and any other file
  // whose size equals a known release asset (a renamed copy). Hashes are cached
  // by name, size and modification time.
  async function scanLocal(signal) {
    await ensure();
    let names = [];
    try { names = (await fs.readdir(folder)).map(String).filter(n => !n.startsWith('.')).slice(0, 512); } catch { names = []; }
    const knownSizes = new Set(), targets = new Set(sources.sources.map(s => s.file));
    for (const check of Object.values(state.checks)) for (const r of check?.releases || []) knownSizes.add(r.asset.size);
    const files = [], hashes = {};
    for (const name of names) {
      let info; try { info = await fs.stat(folder + '/' + name); } catch { continue; }
      if (!info.isFile()) continue;
      const entry = { name, size: info.size, mtimeMs: info.mtimeMs };
      if (targets.has(name) || knownSizes.has(info.size)) {
        const cached = state.hashes[name];
        if (cached && cached.size === info.size && cached.mtimeMs === info.mtimeMs && HEX.test(cached.sha256)) Object.assign(entry, { sha256: cached.sha256, elf: cached.elf });
        else Object.assign(entry, await hashFile(folder + '/' + name, signal), { size: info.size });
        hashes[name] = { size: entry.size, mtimeMs: entry.mtimeMs, sha256: entry.sha256, elf: entry.elf };
      }
      files.push(entry);
    }
    state.hashes = hashes; scan = { at: now(), files };
    try { await saveState(); } catch { /* status still works without the cache */ }
    return files;
  }

  function status() {
    const files = new Map(scan.files.map(f => [f.name, f]));
    const items = sources.sources.map(source => {
      const file = files.get(source.file);
      let local = file ? { name: file.name, size: file.size, sha256: file.sha256 || null } : null;
      if (local && local.size === 0) local.corrupt = 'The file is empty.';
      else if (local && /\.elf$/i.test(source.file) && file.sha256 && file.elf === false) local.corrupt = 'This is not an ELF file: it is damaged or the wrong download.';
      const others = scan.files.filter(f => f.sha256 && f.name !== source.file);
      return { ...classify(source, { check: state.checks[source.id], local, others, install: state.installs[source.id], includePrereleases: config.includePrereleases }), about: source.about };
    });
    return { folder, items, errors: sources.errors, includePrereleases: config.includePrereleases, scannedAt: scan.at, configFile,
      updates: items.filter(i => i.status === 'outdated' || (i.status === 'unknown' && i.latest)).length };
  }

  async function fetchJson(url, context) {
    const file = await runtime.downloads.fetchToFile(url, API_LIMIT, context, { timeoutSeconds: 60 });
    try { return JSON.parse((await fs.readFile(file.source)).toString('utf8')); }
    catch (error) { throw new PmError('ELFUP_RESPONSE', 'GitHub sent an answer that could not be read: ' + clean(error.message, 80)); }
    finally { file.release(); }
  }
  const friendly = error => {
    const message = String(error?.message || error);
    if (/HTTP 403|HTTP 429/.test(error?.action || '') || /rate limit/i.test(message)) return 'GitHub limits checks to 60 an hour per network. Try again later.';
    if (/HTTP 404/.test(error?.action || '')) return 'GitHub has no such repository.';
    return clean(message, 140);
  };

  async function check(input, context) {
    await load();
    const list = input.id ? sources.sources.filter(s => s.id === input.id) : sources.sources;
    invariant(list.length, 'ELFUP_SOURCE', 'This source is no longer configured.');
    let done = 0, failed = 0;
    for (const source of list) {
      invariant(!context.signal?.aborted, 'DOWNLOAD_CANCELLED', 'Check cancelled.');
      await context.progress({ completed: done, total: list.length, phase: 'Checking ' + source.name });
      try {
        const json = await fetchJson(`https://api.github.com/repos/${source.repo}/releases?per_page=${HISTORY}`, context);
        state.checks[source.id] = { checkedAt: now(), repo: source.repo, asset: source.asset, releases: parseReleases(json, source), error: null };
      } catch (error) {
        if (error.code === 'DOWNLOAD_CANCELLED') throw error;
        failed++;
        state.checks[source.id] = { ...(state.checks[source.id] || {}), checkedAt: now(), error: friendly(error) };
      }
      done++;
    }
    await saveState();
    await scanLocal(context.signal);
    const view = status();
    runtime.sequence++;
    const message = failed === list.length ? 'Could not reach GitHub: ' + (state.checks[list[0].id]?.error || 'check the connection.')
      : view.updates ? `${view.updates} update${view.updates === 1 ? '' : 's'} available.` : 'Everything is up to date.';
    return { completed: true, checked: done - failed, failed, updates: view.updates, message };
  }

  // Copy the verified download next to the target, then swap it in. The
  // previous copy is kept as <file>.prev.
  async function place(source, release, downloaded, context) {
    const target = folder + '/' + source.file, temp = target + '.part';
    await fs.mkdir(folder, { recursive: true });
    const input = await fs.open(downloaded, 'r'), output = await fs.open(temp, 'w', 0o777), buffer = Buffer.alloc(1048576);
    const hash = createHash('sha256');
    let at = 0;
    try {
      for (;;) {
        const { bytesRead } = await input.read(buffer, 0, buffer.length, at);
        if (!bytesRead) break;
        await output.writeFile(buffer.subarray(0, bytesRead)); hash.update(buffer.subarray(0, bytesRead)); at += bytesRead;
        await context.progress({ completed: at, total: release.asset.size, phase: 'Writing ' + source.file });
      }
      await output.sync?.();
    } finally { await input.close(); await output.close(); }
    const written = hash.digest('hex');
    if (release.asset.sha256 && written !== release.asset.sha256) {
      try { await fs.unlink(temp); } catch { /* best effort */ }
      throw new PmError('ELFUP_WRITE', 'The copy written to the autoloader folder did not verify. The old file was kept.');
    }
    let previous = false;
    try { await fs.stat(target); previous = true; } catch { previous = false; }
    if (previous) { try { await fs.unlink(target + '.prev'); } catch { /* none yet */ } await fs.rename(target, target + '.prev'); }
    await fs.rename(temp, target);
    return { sha256: written, previous };
  }

  async function install(input, context) {
    await load();
    const source = sources.sources.find(s => s.id === input.id);
    invariant(source, 'ELFUP_SOURCE', 'This source is no longer configured.');
    const releases = state.checks[source.id]?.releases || [];
    invariant(releases.length, 'ELFUP_UNCHECKED', 'Check for updates first.');
    const release = input.tag ? releases.find(r => r.tag === input.tag) : latestRelease(releases, config.includePrereleases);
    invariant(release, 'ELFUP_RELEASE', input.tag ? `Release ${input.tag} is no longer listed. Check for updates again.` : 'No release to install.');
    let file;
    try {
      await context.progress({ completed: 0, total: release.asset.size, phase: `Downloading ${source.name} ${release.tag}` });
      file = await runtime.downloads.fetchToFile(release.asset.url, Math.min(ASSET_LIMIT, release.asset.size + 65536), context);
      await context.progress({ completed: 0, total: release.asset.size, phase: 'Verifying SHA-256' });
      const got = await hashFile(file.source, context.signal);
      invariant(got.size === release.asset.size, 'ELFUP_VERIFY', `The download is ${sizeText(got.size)} but GitHub lists ${sizeText(release.asset.size)}. Nothing was replaced.`);
      invariant(!release.asset.sha256 || got.sha256 === release.asset.sha256, 'ELFUP_VERIFY', 'The download does not match the SHA-256 GitHub published. Nothing was replaced.');
      invariant(!/\.elf$/i.test(source.file) || got.elf, 'ELFUP_VERIFY', 'The download is not an ELF file. Nothing was replaced.');
      const placed = await place(source, release, file.source, context);
      state.installs[source.id] = { tag: release.tag, sha256: placed.sha256, file: source.file, at: now() };
      await saveState();
    } finally { file?.release(); }
    await scanLocal(context.signal);
    runtime.sequence++;
    return { completed: true, id: source.id, tag: release.tag, verified: Boolean(release.asset.sha256),
      message: `${source.name} ${release.tag} installed as ${source.file}${release.asset.sha256 ? ' (SHA-256 verified)' : ''}. It loads on the next boot.` };
  }

  async function installMany(input, context) {
    await load();
    const ids = Array.isArray(input.ids) ? input.ids : [];
    const done = [];
    for (const id of ids) {
      invariant(!context.signal?.aborted, 'DOWNLOAD_CANCELLED', 'Update cancelled.');
      const result = await install({ id }, { ...context, progress: value => context.progress({ ...value, phase: `(${done.length + 1}/${ids.length}) ` + value.phase }) });
      done.push(result.tag ? id + ' ' + result.tag : id);
    }
    return { completed: true, ids, message: `Updated ${done.length} payload${done.length === 1 ? '' : 's'}. They load on the next boot.` };
  }

  async function setPrereleases(on) {
    await ensure();
    config.includePrereleases = Boolean(on);
    const raw = await readJson(configFile, {});
    if (!raw.$error) await writeJson(configFile, { ...raw, includePrereleases: config.includePrereleases });
    runtime.sequence++;
    return status();
  }

  const reconcile = async () => ({ absent: true });
  const idInput = input => { invariant(input && typeof input.id === 'string' && ID.test(input.id), 'ELFUP_SOURCE', 'Choose a payload.'); return input; };
  runtime.jobs.register('elfupdater.check', {
    validate: input => input?.id ? { id: idInput(input).id } : {},
    resources: () => ['native-download', 'elf-updater'], timeout: 10 * 60 * 1000, cancellable: true,
    run: check, verify: async () => true, reconcile
  });
  runtime.jobs.register('elfupdater.install', {
    validate(input) {
      const { id } = idInput(input);
      invariant(input.tag == null || (typeof input.tag === 'string' && input.tag.length <= 64), 'ELFUP_RELEASE', 'Invalid release.');
      return input.tag ? { id, tag: input.tag } : { id };
    },
    resources: () => ['native-download', 'elf-updater', 'autoload-folder'], timeout: 20 * 60 * 1000, cancellable: true,
    run: install, verify: async () => true, reconcile
  });
  runtime.jobs.register('elfupdater.update-all', {
    validate(input) {
      invariant(Array.isArray(input?.ids) && input.ids.length > 0 && input.ids.length <= 32 && input.ids.every(id => typeof id === 'string' && ID.test(id)), 'ELFUP_SOURCE', 'Nothing to update.');
      return { ids: [...new Set(input.ids)] };
    },
    resources: () => ['native-download', 'elf-updater', 'autoload-folder'], timeout: 60 * 60 * 1000, cancellable: true,
    run: installMany, verify: async () => true, reconcile
  });

  runtime.elfUpdater = { load, scanLocal, status, setPrereleases, releases: id => [...(state.checks[id]?.releases || [])], get sources() { return sources.sources; } };
  return runtime.elfUpdater;
}
