// SPDX-License-Identifier: GPL-3.0-or-later
import { clone, invariant, PmError, publicError } from './errors.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hash = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
const integer = value => Number.isSafeInteger(value) && value >= 0;
const max = 32 * 1024 ** 3, maxArchive = max + 32 * 1024 ** 2;
const clean = (code, message) => Object.assign(new PmError(code, message), { noSideEffects: true });
function save(value) {
  invariant(value && typeof value.user === 'string' && /^[a-f0-9]{1,8}$/.test(value.user) &&
    [4, 5].includes(value.platform) && typeof value.titleId === 'string' &&
    (value.platform === 5 ? /^(PPSA|PPSB|MOUU)\d{5}$/ : /^(CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/).test(value.titleId) &&
    typeof value.slot === 'string' && /^sdimg_[A-Za-z0-9_.-]{1,121}$/.test(value.slot) &&
    !['sdimg_.', 'sdimg_..'].includes(value.slot) && !value.slot.startsWith('sdimg_sce_bu_') &&
    hash(value.stamp) && integer(value.bytes) && value.bytes >= 0x860 && value.bytes <= max,
  'SAVE_CATALOG', 'A save catalog entry is invalid.');
  return { user: value.user, platform: value.platform, titleId: value.titleId, slot: value.slot, stamp: value.stamp, bytes: value.bytes };
}
export function installSaves(runtime, native, { delay = sleep, now = Date.now, recoveryInterval = 1000 } = {}) {
  const available = ['saveStart', 'savePoll', 'saveCatalog', 'systemProbe'].every(key => typeof native[key] === 'function');
  let catalog = null;
  const recoveryAvailable = typeof native.saveRecoverStart === 'function' && typeof native.saveRecoverPoll === 'function';
  const recovering = new Set();let recoveryTimer = null, stopped = false;
  function watchRecovery() {
    if (stopped || recoveryTimer || !recovering.size) return;
    recoveryTimer = setTimeout(async () => {
      recoveryTimer = null;
      for (const id of [...recovering]) {
        if (stopped) break;
        if (runtime.jobs.snapshot().find(job => job.id === id)?.status !== 'interrupted') { recovering.delete(id); continue; }
        try { await runtime.jobs.reconcile(id); }
        catch (error) {
          recovering.delete(id);
          try { await runtime.store.transaction(state => {
            const job=state.jobs.find(item=>item.id===id);
            if(job?.status==='interrupted'){job.error=publicError(error);job.updatedAt=now();}
          });runtime.sequence++; } catch (commitError) { runtime.jobs.halt(commitError); }
        }
      }
      watchRecovery();
    }, recoveryInterval);
    recoveryTimer.unref?.();
  }
  function recoveryStatus(id) {
    const status = native.saveRecoverPoll(id);
    invariant(status?.request === id && typeof status.running === 'boolean' && typeof status.finished === 'boolean' &&
      status.running !== status.finished && typeof status.ok === 'boolean' && integer(status.completed) && integer(status.total) &&
      status.total <= maxArchive && status.completed <= status.total && typeof status.message === 'string' && status.message.length <= 191,
    'SAVE_RECOVERY_RECEIPT', 'The background save check has no matching status; the operation stays held.');
    return status;
  }
  runtime.capability('saves', available, available
    ? 'PS4/PS5 staged-copy decryption and verified USB/FTP exports. Console validation is pending.'
    : 'Native save helper is unavailable in this build.');
  const empty = () => ({ mode: 1, user: '', titleId: '', slot: '', stamp: '', destination: 0, volume: '', platform: 0 });
  function readCatalog(request) {
    const raw = native.saveCatalog(request);
    invariant(typeof raw === 'string' && raw.length <= 2 * 1024 ** 2, 'SAVE_CATALOG', 'Save catalog is unavailable or too large.');
    const parsed = JSON.parse(raw);
    invariant(parsed?.version === 1 && Array.isArray(parsed.saves) && parsed.saves.length <= 2048 &&
      Array.isArray(parsed.volumes) && parsed.volumes.length <= 8 && integer(parsed.skipped), 'SAVE_CATALOG', 'Save catalog is incomplete.');
    const entries = parsed.saves.map(save), keys = new Set();
    for (const item of entries) { const key = `${item.user}/${item.titleId}/${item.slot}`; invariant(!keys.has(key), 'SAVE_CATALOG', 'Duplicate save identity.'); keys.add(key); }
    const volumes = parsed.volumes.map(v => {
      invariant(integer(v.destination) && v.destination >= 1 && v.destination <= 8 && v.path === `/mnt/usb${v.destination - 1}` && hash(v.stamp) && integer(v.freeBytes),
        'SAVE_CATALOG', 'USB destination identity is invalid.');
      return { destination: v.destination, path: v.path, stamp: v.stamp, freeBytes: v.freeBytes };
    });
    invariant(new Set(volumes.map(v => v.destination)).size === volumes.length, 'SAVE_CATALOG', 'Duplicate USB destination.');
    catalog = { request, entries, volumes, skipped: parsed.skipped, updatedAt: now() }; runtime.sequence++;
    return catalog;
  }
  async function validate(input, mode) {
    invariant(available, 'SAVE_UNAVAILABLE', 'Save export requires the native save helper.');
    invariant(!runtime.graph.safe, 'SAFE_STARTUP', 'Leave recovery startup before starting a save helper.');
    if (mode === 1) return empty();
    invariant(catalog, 'SAVE_REFRESH', 'Refresh the save list before choosing an export.');
    const selected = catalog.entries.find(item => item.user === input?.user && item.titleId === input.titleId && item.slot === input.slot && item.stamp === input.stamp);
    invariant(selected, 'SAVE_CHANGED', 'The chosen save changed. Refresh and select it again.');
    invariant(!runtime.game || runtime.game.titleId !== selected.titleId, 'SAVE_GAME_RUNNING', 'Close this game before exporting its save.');
    invariant(integer(input.destination) && input.destination <= 8, 'SAVE_DESTINATION', 'Choose console storage or a listed USB drive.');
    const volume = input.destination ? catalog.volumes.find(v => v.destination === input.destination && v.stamp === input.volume) : null;
    invariant(!input.destination || volume, 'SAVE_VOLUME_CHANGED', 'USB selection changed. Refresh the save list.');
    return { mode: 2, ...selected, destination: input.destination, volume: volume?.stamp || '' };
  }
  function receipt(request, input) {
    const r = native.savePoll(request);
    invariant(r && r.request === request && ['mode', 'user', 'titleId', 'slot', 'stamp', 'destination', 'volume', 'platform'].every(key => r[key] === input[key]) &&
      Number.isInteger(r.stage) && r.stage >= 1 && r.stage <= 10 && integer(r.ageMs) && typeof r.sameBoot === 'boolean' &&
      typeof r.mounted === 'boolean' && typeof r.noSideEffects === 'boolean' && integer(r.totalBytes) && r.totalBytes <= maxArchive &&
      integer(r.completedBytes) && r.completedBytes <= r.totalBytes && integer(r.archiveBytes) && r.archiveBytes <= maxArchive &&
      integer(r.files) && r.files <= 10000 && typeof r.message === 'string' && r.message.length <= 191,
    'SAVE_RECEIPT', 'Save operation receipt does not match this request. No retry was issued.');
    return r;
  }
  function outcome(r, input) {
    if (r.stage !== 8) return null;
    invariant(r.noSideEffects && !r.mounted && hash(r.sha256) && r.archiveBytes > 0, 'SAVE_RECEIPT', 'Save completion lacks verified unmount or checksum evidence.');
    const output = input.mode === 1 ? `/data/ProsperoMenu/save-export/${r.request}.catalog.json` :
      `${input.destination ? `/mnt/usb${input.destination - 1}/ProsperoMenu/Saves` : '/data/ProsperoMenu/share/saves'}/${r.request}/save.tar`;
    invariant(r.output === output, 'SAVE_RECEIPT', 'Save receipt refers to an unexpected destination.');
    if (input.mode === 1) {
      const listing = readCatalog(r.request);
      return { completed: true, request: r.request, mode: 1, files: listing.entries.length, message: 'Save list refreshed.', sha256: r.sha256 };
    }
    return { completed: true, request: r.request, mode: 2, titleId: input.titleId, slot: input.slot,
      output, ftpPath: input.destination ? null : `/saves/${r.request}/save.tar`, sha256: r.sha256, bytes: r.archiveBytes,
      files: r.files, message: 'Save backup created and verified.' };
  }
  for (const [kind, mode] of [['save.refresh', 1], ['save.export', 2]]) runtime.jobs.register(kind, {
    validate: input => validate(input, mode), resources: () => ['save-export', 'native-payload-dispatch'], timeout: 12 * 60 * 60 * 1000, cancellable: false,
    async run(input, context) {
      try { await validate(input, mode); } catch (error) { error.noSideEffects = true; throw error; }
      const deadline = now() + 60000;
      for (;;) {
        if (runtime.graph.safe) throw clean('SAFE_STARTUP', 'Recovery startup now holds optional native work; no save helper was started.');
        const probe = native.systemProbe();
        if (!probe.busy) {
          if (probe.kstuff?.state !== 'installed' || !probe.kstuff.injectionReady) throw clean('SAVE_DEPENDENCY', 'Verified kstuff must be ready before save export.');
          try { native.saveStart(context.id, mode, input.user, input.titleId, input.slot, input.stamp, input.destination, input.volume, input.platform); break; }
          catch (error) {
            if (error.code !== 'NATIVE_BUSY' || error.noSideEffects !== true) {
              // Native errors are ordinary Error objects, not PmError. Retain
              // this bounded adapter's explanation instead of a generic failure.
              if (['SAVE_NATIVE', 'SAVE_UNRESOLVED'].includes(error.code)) {
                const detail = new PmError(error.code, error.message, { action: error.code === 'SAVE_UNRESOLVED'
                  ? 'Open Transfers and check the previous save operation.'
                  : 'Refresh saves again. If this repeats, keep the console on for diagnostics.' });
                detail.noSideEffects = error.noSideEffects === true; throw detail;
              }
              throw error;
            }
          }
        }
        if (now() >= deadline || context.signal.aborted) throw clean('NATIVE_BUSY', 'Another native operation is active; no save helper was started.');
        await delay(250);
      }
      await context.checkpoint({ request: context.id, mode, stamp: input.stamp });
      for (;;) {
        const r = receipt(context.id, input); context.evidence?.(r);
        const result = outcome(r, input); if (result) return result;
        if (r.stage === 9 && r.noSideEffects && !r.mounted) throw clean('SAVE_REJECTED', r.message);
        invariant(r.stage !== 10 && r.sameBoot && r.ageMs <= 90000 && !context.signal.aborted,
          'SAVE_UNCERTAIN', r.stage === 10 ? r.message : 'Save helper stopped reporting. Staged files are retained; no retry was issued.');
        await context.progress({ completed: r.completedBytes, total: r.totalBytes, phase: r.message }); await delay(500);
      }
    },
    verify(result, input) { const current = outcome(receipt(result.request, input), input); return current?.sha256 === result.sha256 && current.mode === result.mode; },
    reconcile(job) {
      const r = receipt(job.id, job.args);
      const result = outcome(r, job.args);
      if (result || r.stage === 9 && r.noSideEffects && !r.mounted) {
        recovering.delete(job.id);
        return result || { absent: true, message: r.message, stagingRetained: true };
      }
      if (!recoveryAvailable) return null;
      if (recovering.has(job.id)) {
        const check = recoveryStatus(job.id);
        if (check.running) return { pending: true, completed: check.completed, total: check.total, message: 'Checking the retained save export in the background.' };
        recovering.delete(job.id);
        invariant(check.ok, 'SAVE_RECOVERY_HELD', check.message || 'Save recovery could not verify a safe outcome.');
        // Completion may have committed between reading the receipt and status.
        const latest = receipt(job.id, job.args), done = outcome(latest, job.args);
        if (done) return done;
        if (latest.stage === 9 && latest.noSideEffects && !latest.mounted) return { absent: true, message: latest.message, stagingRetained: true };
        throw new PmError('SAVE_RECOVERY_HELD', 'The background check finished without a verified save result.');
      }
      invariant(hash(r.receiptHash), 'SAVE_RECOVERY_RECEIPT', 'This receipt cannot be bound to a background recovery check.');
      try { native.saveRecoverStart(job.id, r.receiptHash); }
      catch (error) { throw new PmError('SAVE_RECOVERY_HELD', error.code === 'NATIVE_BUSY'
        ? 'Another native operation is active. Check this save result again after it finishes.'
        : 'The save receipt or helper state changed. Check the result again; no export was restarted.'); }
      recovering.add(job.id);watchRecovery();
      return { pending: true, completed: 0, total: 0, message: 'Checking the retained save export in the background.' };
    }
  });
  runtime.saves = {
    snapshot: () => ({ available, recoveryAvailable, recovering: [...recovering], count: catalog?.entries.length ?? 0, updatedAt: catalog?.updatedAt ?? null, skipped: catalog?.skipped ?? 0 }),
    stop() { stopped = true;if (recoveryTimer) clearTimeout(recoveryTimer);recoveryTimer = null; },
    list({ query = '', offset = 0, limit = 30 } = {}) {
      invariant(typeof query === 'string' && query.length <= 128 && integer(offset) && integer(limit) && limit >= 1 && limit <= 50, 'SAVE_QUERY', 'Invalid save list request.');
      const entries = (catalog?.entries || []).filter(item => `${item.titleId} ${item.slot} ${item.user}`.toLowerCase().includes(query.toLowerCase()));
      return { entries: clone(entries.slice(offset, offset + limit)), total: entries.length, offset, limit,
        volumes: clone(catalog?.volumes || []), updatedAt: catalog?.updatedAt ?? null, skipped: catalog?.skipped ?? 0 };
    }
  };
  return runtime.saves;
}
