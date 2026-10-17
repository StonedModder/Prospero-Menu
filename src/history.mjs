// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { canonical, clone, invariant } from './errors.mjs';
import { validateProfile } from './profiles.mjs';
const digest = value => createHash('sha256').update(canonical(value)).digest('hex');
const equal = (a, b) => canonical(a) === canonical(b);
const sameGame = (a, b) => b && ['pid', 'generation', 'titleId', 'version', 'metadataHash'].every(key => a[key] === b[key]);
const fields = patch => Object.entries(patch).flatMap(([group, values]) => Object.keys(values).map(key => `${group}.${key}`));
const proof = entry => digest({ id: entry.id, session: entry.session, change: entry.change, target: entry.target, before: entry.before, after: entry.after });
const jobProof = job => digest({ id: job.id, session: job.session, kind: job.kind, signature: job.signature, result: job.result });
const preferenceNames = { 'overlay.fps': 'FPS overlay', 'overlay.ram': 'RAM overlay', 'overlay.cpu': 'CPU overlay',
  'overlay.ip': 'Console IP overlay', 'overlay.adaptive': 'Adaptive notices', 'appearance.accent': 'Accent color',
  'appearance.reducedMotion': 'Reduced motion', 'services.ftp': 'FTP at startup', 'services.inboxAutoRun': 'Payload inbox autostart',
  'services.shadowmount': 'ShadowMount at startup', 'services.payloadManager': 'Payload Manager at startup' };
const display = value => typeof value === 'boolean' ? value ? 'on' : 'off' : String(value);
const names = { 'cheat.set': 'Game cheat changed', 'library.mount': 'Game mount changed', 'plugin.start': 'Plugin process loaded',
  'game.module.start': 'Game module loaded', 'payload.dispatch': 'Payload dispatched', 'loadout.apply': 'Game loadout',
  'package.install': 'Package installation', 'package.verify': 'Package verified', 'save.refresh': 'Save list refreshed',
  'save.export': 'Save export', 'library.storage': 'Game storage operation', 'memory.scan': 'Memory scan',
  'game.input.prepare': 'In-game controls', 'fps.ps4.prepare': 'PS4 FPS counter', 'deck.start': 'Menu renderer',
  'component.start': 'Component start', 'library.launch': 'Game launch', 'payload.load': 'Payload load' };

export class SessionHistory {
  constructor(runtime) { this.runtime = runtime; }
  record(state, { change, target = '', before = null, after = null, message, detail = '', reversalOf = null }) {
    if (equal(before, after)) return null;
    const entry = { format: 'prosperomenu-change-v1', id: this.runtime.id(), session: this.runtime.session, at: this.runtime.now(),
      type: 'configuration', change, target, before: clone(before), after: clone(after), message, detail, reversalOf, undone: null };
    state.history.push(entry);
    while (state.history.length > 256 || new TextEncoder().encode(JSON.stringify(state.history)).length > 512 * 1024) state.history.shift();
    return entry;
  }
  settingsChanged(state, before, after, reversalOf = null) {
    const old = {}, next = {};
    for (const [group, values] of Object.entries(after)) for (const [field, value] of Object.entries(values)) if (before[group][field] !== value) {
      (old[group] ||= {})[field] = before[group][field];(next[group] ||= {})[field] = value;
    }
    return this.record(state, { change: 'settings', before: old, after: next,
      message: reversalOf ? 'Previous preferences restored' : 'Preferences changed',
      detail: Object.keys(next).includes('services') ? 'Service choices are saved preferences; this does not stop an already loaded service.' : '', reversalOf });
  }
  current(entry, state) {
    if (entry.change === 'settings') {
      const out = {};
      for (const [group, values] of Object.entries(entry.after)) for (const field of Object.keys(values)) {
        invariant(Object.hasOwn(state.settings, group) && Object.hasOwn(state.settings[group], field), 'HISTORY_INVALID', 'Recorded preference is invalid.');
        (out[group] ||= {})[field] = state.settings[group][field];
      }return out;
    }
    if (entry.change === 'plugin-config') return state.plugins.find(p => p.sha256 === entry.target)?.autostart ?? null;
    if (entry.change === 'profile') return state.profiles[entry.target] ?? null;
    return null;
  }
  overlaps(a, b) {
    if (a.change !== b.change) return false;
    if (a.change === 'settings') { const used = new Set(fields(a.after));return fields(b.after).some(key => used.has(key)); }
    return a.target === b.target;
  }
  configPlan(entry, state) {
    let reason = '';
    if (entry.session !== this.runtime.session) reason = 'Only changes from this runtime session can be reversed.';
    else if (entry.undone) reason = 'This change has already been reversed.';
    else if (entry.reversalOf) reason = 'This records a reversal. Use the ordinary controls to make a new change.';
    else if (!['settings', 'plugin-config', 'profile'].includes(entry.change)) reason = 'This change has no automatic reversal.';
    else if (entry.change === 'plugin-config') reason = 'Plugin management has been removed. This legacy choice cannot be restored.';
    else if (entry.change === 'settings' && Object.hasOwn(entry.after, 'developer')) reason = 'Developer grants must be enabled explicitly; undo does not restore access.';
    else if (!equal(this.current(entry, state), entry.after)) reason = 'These choices changed afterward. Undo would overwrite a newer change.';
    else {
      const index = state.history.findIndex(item => item.id === entry.id);
      if (state.history.slice(index + 1).some(later => later.format === 'prosperomenu-change-v1' && this.overlaps(entry, later) && !later.undone && !later.reversalOf))
        reason = 'A later change touches the same choices. Reverse that change first.';
    }
    return { available: !reason, reason, proof: proof(entry), label: entry.change === 'settings' ? 'Restore previous preferences' :
      entry.change === 'profile' ? 'Restore previous loadout' : 'Restore previous autostart',
      detail: entry.change === 'plugin-config' ? 'Restore the previous autostart choice. Enabling it can arm a future launch; loaded code remains resident.' :
        entry.change === 'profile' ? 'Restore the previous saved loadout. Its automatic-start choice may arm a future run; already applied native steps stay applied.' :
          'Restore only the listed preferences. Other choices stay as they are.',
      changes: entry.change === 'settings' ? fields(entry.before).map(name => { const [group, field] = name.split('.');return `${preferenceNames[name] || name}: ${display(entry.after[group][field])} → ${display(entry.before[group][field])}`; }) :
        entry.change === 'plugin-config' ? [`Autostart: ${entry.after ? 'on' : 'off'} → ${entry.before ? 'on' : 'off'}`] :
          entry.change === 'profile' && entry.before ? [`Restore ${entry.before.name}`, `${entry.before.titleId} · ${entry.before.version}`,
            `Automatic loadout: ${display(entry.before.autostart)}`, `${entry.before.plugins.length} plugins · ${entry.before.cheats.length} cheats`,
            ...Object.entries(entry.before.overlay).map(([key, value]) => `${preferenceNames['overlay.' + key]}: ${display(value)}`)] :
            entry.change === 'profile' ? ['Remove this saved loadout'] : [] };
  }
  jobPlan(job, state, currentId = null) {
    const reversal = state.jobs.find(j => j.args?.undoOf?.id === job.id);
    let reason = '';
    if (job.session !== this.runtime.session) reason = 'Only operations from this runtime session can be reversed.';
    else if (job.args?.undoOf) reason = 'This is a reversal operation. Use the ordinary controls for another change.';
    else if (reversal && reversal.id !== currentId) reason = reversal.status === 'completed' && reversal.result?.phase !== 'game-ended'
      ? 'Reversal completed.' : `Reversal ${reversal.status}; inspect its existing operation in Activity.`;
    else if (job.status !== 'completed') reason = 'The original operation has not completed and been verified.';
    else if (this.runtime.graph.safe) reason = 'Native reversals are held during recovery startup.';
    else if (job.kind === 'cheat.set') {
      if (job.result?.phase !== 'applied') reason = 'A successful live patch was not confirmed.';
      else if (job.args.exactUndo === false) reason = 'This cheat uses different On/Off lengths. Use its ordinary disable control; exact history undo is unavailable.';
      else if (!sameGame(job.args, this.runtime.game)) reason = 'The original game instance ended or changed.';
    } else if (job.kind === 'library.mount') {
      if (job.args.priorMounted === job.args.mounted) reason = 'The game already had the requested mount state; there is no change to undo.';
      else if (this.runtime.game?.titleId === job.args.titleId) reason = 'Close this game before changing its mount.';
    } else reason = this.effectReason(job);
    if (!reason) {
      // Reverse a stack of native changes in order. Completed reversals cancel
      // their own original operation; they do not count as another active patch.
      const undone = new Set(state.jobs.filter(j => j.status === 'completed' && j.args?.undoOf &&
        (j.kind !== 'cheat.set' || j.result?.phase === 'applied')).map(j => j.args.undoOf.id));
      const later = state.jobs.slice(state.jobs.findIndex(j => j.id === job.id) + 1).filter(j => j.id !== currentId && j.kind === job.kind &&
        (job.kind === 'cheat.set' ? j.args.generation === job.args.generation : j.args.titleId === job.args.titleId));
      if (later.some(j => !['failed', 'cancelled'].includes(j.status) && !(j.args?.undoOf && j.status === 'completed') &&
        !(j.kind === 'library.mount' && j.status === 'completed' && j.args.priorMounted === j.args.mounted) && !undone.has(j.id)))
        reason = 'Later work affects the same game. Resolve or reverse it first.';
    }
    const label = job.kind === 'cheat.set' ? (job.args.enabled ? 'Restore pre-cheat bytes' : 'Reapply the previous cheat state') : 'Restore previous game mount';
    return { available: !reason, reason, proof: jobProof(job), label,
      detail: job.kind === 'cheat.set' ? 'The exact game instance and expected current bytes will be checked again before writing.' :
        'The original ShadowMount instance, source identity and current mount state will be checked again.',
      changes: job.kind === 'cheat.set' ? [`${job.args.titleId} · ${job.args.version}`, `Cheat: ${display(job.args.enabled)} → ${display(!job.args.enabled)}`] :
        job.kind === 'library.mount' ? [job.args.titleId, job.args.priorMounted ? 'Restore mounted state' : 'Restore unmounted state'] : [] };
  }
  effectReason(job) {
    if (['plugin.start', 'game.module.start', 'payload.dispatch'].includes(job.kind)) return 'Loaded code has no generic undo. Disarming autostart does not unload it or restore patches.';
    if (job.kind === 'loadout.apply') return 'Reverse supported steps individually. Loaded plugins remain; release the temporary overlay in Games.';
    if (job.kind === 'save.export') return 'This export left the installed save unchanged. Removing an exported copy is a separate file operation.';
    if (job.kind === 'package.install') return 'Uninstalling a package is a separate action, not an automatic rollback.';
    if (job.kind === 'library.storage') return 'Storage changes require a separate reviewed operation; source deletion is not inferred.';
    return 'No reversible state change is recorded for this operation.';
  }
  lookup(id, state = this.runtime.store.snapshot()) {
    invariant(typeof id === 'string' && /^[a-f0-9]{32}$/.test(id), 'HISTORY_ID', 'Choose a recorded change.');
    const entry = state.history.find(e => e.id === id && e.format === 'prosperomenu-change-v1');
    if (entry) return { entry, plan: this.configPlan(entry, state), state };
    const job = state.jobs.find(j => j.id === id);
    invariant(job, 'HISTORY_MISSING', 'This change is no longer in history.');return { job, plan: this.jobPlan(job, state), state };
  }
  preview({ id }) { const { entry, job, plan } = this.lookup(id);return { id, message: entry?.message || names[job.kind] || job.kind, ...plan }; }
  snapshot(state = this.runtime.store.snapshot()) {
    const game = this.runtime.game;
    const cacheKey = canonical([state.revision, this.runtime.session, this.runtime.graph.safe,
      game && [game.pid, game.generation, game.titleId, game.version, game.metadataHash]]);
    if (this.cache?.key === cacheKey) return clone(this.cache.entries);
    const sources = [
      ...state.history.filter(e => e.session === this.runtime.session).map(entry => ({ entry, at: entry.at })),
      ...state.jobs.filter(j => j.session === this.runtime.session && !['queued', 'running', 'cancelling'].includes(j.status))
        .map(job => ({ job, at: job.updatedAt }))
    ].sort((a, b) => a.at - b.at).slice(-256);
    const entries = sources.map(({ entry, job }) => entry ? ({ id: entry.id, at: entry.at,
      type: entry.type, message: entry.message, detail: entry.detail || entry.fields?.join(', ') || '',
      undo: entry.format === 'prosperomenu-change-v1' ? this.configPlan(entry, state) : { available: false, reason: 'This event is informational.' } }) :
      ({ id: job.id, at: job.updatedAt, type: 'job', jobId: job.id, status: job.status,
        message: (job.args.undoOf ? 'Reversal: ' : '') + (names[job.kind] || job.kind),
        detail: job.error?.message || job.result?.message || job.progress?.phase || job.status, undo: this.jobPlan(job, state) }));
    this.cache = { key: cacheKey, entries };return clone(entries);
  }
  configGuard(entry, expectedProof, state) {
    invariant(proof(entry) === expectedProof, 'HISTORY_CHANGED', 'The reviewed change differs from this undo request.');
    const plan = this.configPlan(entry, state);invariant(plan.available, 'HISTORY_CONFLICT', plan.reason);
  }
  guardJobUndo(input, kind, { state = this.runtime.store.snapshot(), jobId = null } = {}) {
    if (!input.undoOf) return;
    const reference = input.undoOf, source = state.jobs.find(j => j.id === reference.id);
    invariant(source && source.kind === kind && jobProof(source) === reference.proof, 'HISTORY_CHANGED', 'The original operation differs from this reversal request.');
    const plan = this.jobPlan(source, state, jobId);invariant(plan.available, 'HISTORY_CONFLICT', plan.reason);
    if (kind === 'cheat.set') invariant(input.sha256 === source.args.sha256 && input.cheatId === source.args.cheatId && input.enabled === !source.args.enabled &&
      sameGame(input, source.args) && input.boot === source.args.boot, 'HISTORY_TARGET', 'The reversal must target the original cheat and game instance.');
    else invariant(input.titleId === source.args.titleId && input.path === source.args.path && input.contentId === source.args.contentId &&
      input.mounted === source.args.priorMounted && input.priorMounted === source.args.mounted && equal(input.service, source.args.service),
    'HISTORY_TARGET', 'The reversal must target the original mount and service instance.');
  }
  async undo({ id, proof: expectedProof }) {
    const found = this.lookup(id), { entry, job } = found;
    invariant(typeof expectedProof === 'string' && expectedProof === found.plan.proof, 'HISTORY_CHANGED', 'Review the current change before undoing it.');
    if (entry?.undone) return { reversed: true, id, message: 'This change was already reversed.' };
    if (job) {
      const existing = found.state.jobs.find(j => j.args?.undoOf?.id === id);
      if (existing) return { accepted: true, jobId: existing.id, message: 'The existing reversal is shown in Activity.' };
      invariant(found.plan.available, 'HISTORY_CONFLICT', found.plan.reason);
      const reference = { id, proof: expectedProof };
      const input = job.kind === 'cheat.set' ? { sha256: job.args.sha256, cheatId: job.args.cheatId, enabled: !job.args.enabled, undoOf: reference } :
        { titleId: job.args.titleId, path: job.args.path, contentId: job.args.contentId, service: job.args.service, mounted: job.args.priorMounted, undoOf: reference };
      let accepted;
      try { accepted = await this.runtime.jobs.enqueue(job.kind, input, 'undo-' + id); }
      catch (error) {
        // Another device may have accepted the identical reversal while this
        // request was validating its live target. Adopt that intent, never retry.
        accepted = this.runtime.store.snapshot().jobs.find(j => j.key === 'undo-' + id && j.args?.undoOf?.proof === expectedProof);
        if (!accepted) throw error;
      }
      return { accepted: true, jobId: accepted.id, message: 'Reversal queued; completion is tracked in Activity.' };
    }
    if (entry.change === 'settings') return this.runtime.restorePreferences(id, expectedProof);
    await this.runtime.store.transaction(state => {
      const current = state.history.find(e => e.id === id);invariant(current, 'HISTORY_MISSING', 'This change is no longer in history.');
      if (current.undone) return;this.configGuard(current, expectedProof, state);
      if (current.change === 'plugin-config') state.plugins.find(p => p.sha256 === current.target).autostart = current.before;
      else if (current.change === 'profile') {
        if (current.before) { const profile = validateProfile(current.before);invariant(current.target === `${profile.titleId}@${profile.version}`, 'HISTORY_TARGET', 'Loadout identity changed.');state.profiles[current.target] = profile; }
        else delete state.profiles[current.target];
      } else invariant(false, 'HISTORY_UNSUPPORTED', 'This change has no supported reversal.');
      current.undone = { at: this.runtime.now() };
      this.record(state, { change: current.change, target: current.target, before: current.after, after: current.before,
        message: 'Previous ' + (current.change === 'profile' ? 'loadout' : 'plugin autostart') + ' restored', detail: current.detail, reversalOf: id });
    });
    this.runtime.sequence++;return { reversed: true, id, message: 'Previous saved choices restored. Existing native effects were not unloaded.' };
  }
}
