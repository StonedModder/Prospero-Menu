// SPDX-License-Identifier: GPL-3.0-or-later
import { clone, invariant } from './errors.mjs';
const names = { 'payload.dispatch': 'Payload', 'plugin.start': 'Plugin', 'game.module.start': 'Game module',
  'cheat.set': 'Cheat', 'loadout.apply': 'Game loadout', 'package.verify': 'Package verification', 'package.install': 'Package installation',
  'library.mount': 'Game mount', 'library.storage': 'Game files', 'save.refresh': 'Save list', 'save.export': 'Save export',
  'game.input.prepare': 'In-game controls', 'fps.ps4.prepare': 'PS4 FPS counter' };
const problems = new Set(['failed', 'interrupted', 'uncertain', 'held']);
const clean = value => String(value || '').replace(/[\x00-\x1f\x7f]/g, ' ').slice(0, 240);

// Passive presentation only. This observer never starts/retries native work and
// owns no timer. Existing state publication supplies observations and clock ticks.
export class AdaptiveNotices {
  constructor(runtime) {
    this.runtime = runtime;this.jobs = new Map();this.services = new Map();this.pending = [];this.halted = false;
    this.current = null;this.sequence = 1;this.initialized = false;this.enabled = false;this.lastNow = null;this.nextAt = 0;
  }
  bump() { this.sequence = (this.sequence + 1) >>> 0 || 1; }
  emit(group, level, title, detail, reference, now) {
    if (!this.enabled) return;
    const item = { id: this.runtime.id(), group, level, title: clean(title), detail: clean(detail), count: 1, reference, createdAt: now, expiresAt: now + 30000 };
    // Prefer attention over success; group a burst without extending its display.
    const existing = this.current?.group === group ? this.current : this.pending.find(n => n.group === group);
    if (existing) {
      existing.count = Math.min(999, existing.count + 1);existing.detail = group === 'services-attention'
        ? 'Several services need attention. Review Components.' : 'Several updates. Open Activity to review each result.';
      existing.reference = null;this.bump();return;
    }
    if (level === 'attention' && (!this.current || this.current.level === 'success')) { this.current = null;this.nextAt = now; }
    this.pending.push(item);this.pending.sort((a, b) => (a.level === 'attention' ? 0 : 1) - (b.level === 'attention' ? 0 : 1));
    this.pending = this.pending.slice(0, 6);
  }
  observe(state = this.runtime.store.snapshot(), services = this.runtime.graph.snapshot()) {
    if (!this.runtime.started) return;
    const now = this.runtime.now();
    if (this.lastNow !== null && now < this.lastNow) { this.current = null;this.pending = [];this.nextAt = now;this.bump(); }
    this.lastNow = now;
    const enabled = state.settings.overlay.adaptive;
    if (this.enabled !== enabled) {
      this.enabled = enabled;this.current = null;this.pending = [];this.nextAt = now;this.bump();
    }
    // Expire presentation before grouping a newly observed event. Otherwise a
    // long idle gap could absorb a new failure into an already expired popup.
    this.advance(now);
    for (const job of state.jobs) {
      const previous = this.jobs.get(job.id);this.jobs.set(job.id, job.status);
      if (!this.initialized || job.session !== this.runtime.session || previous === job.status || !names[job.kind]) continue;
      if (problems.has(job.status)) {
        if (problems.has(previous)) continue; // timeout -> interrupted is one incident
        this.emit('jobs-attention', 'attention', 'An operation needs attention',
          `${names[job.kind]}: ${job.error?.message || 'Its result needs checking.'} Open Activity.`, { type: 'job', id: job.id }, now);
      } else if (job.status === 'completed') {
        if (job.result?.phase === 'loadout-stopped' || job.result?.phase === 'game-ended')
          this.emit('jobs-attention', 'attention', names[job.kind] + ' needs review', job.result.message || 'Review the recorded result in Activity.', { type: 'job', id: job.id }, now);
        else if (!['save.refresh', 'game.input.prepare', 'fps.ps4.prepare'].includes(job.kind)) {
          const dispatched = ['dispatched', 'plugin-dispatched'].includes(job.result?.phase);
          this.emit('jobs-success', 'success', job.args?.undoOf ? 'Reversal completed' : names[job.kind] + (dispatched ? ' dispatched' : ' completed'),
            dispatched ? 'The loader dispatched it. Feature initialization is checked separately.' : 'Verified result available in Activity.', { type: 'job', id: job.id }, now);
        }
      }
    }
    const ids = new Set(state.jobs.map(j => j.id));for (const id of this.jobs.keys()) if (!ids.has(id)) this.jobs.delete(id);
    for (const service of services) {
      const previous = this.services.get(service.id);this.services.set(service.id, service.status);
      if (!this.initialized || previous === service.status) continue;
      if (problems.has(service.status) || service.status === 'blocked') {
        if (problems.has(previous) || previous === 'blocked') continue;
        this.emit('services-attention', 'attention', 'A service needs attention',
          `${service.id}: ${service.error?.message || 'Readiness was lost.'} Review Components.`, { type: 'service', id: service.id }, now);
      }
    }
    if (!this.initialized && this.runtime.graph.safe) this.emit('recovery', 'attention', 'Recovery startup',
      'Optional startup is held. Review the previous operations in Activity.', null, now);
    if (this.initialized && this.runtime.jobs.halted && !this.halted) this.emit('jobs-attention', 'attention', 'Background work paused',
      this.runtime.jobs.fault?.message || 'The queue stopped accepting work. Review Activity before retrying.', null, now);
    this.halted = this.runtime.jobs.halted;
    this.initialized = true;this.advance(now);
  }
  advance(now) {
    if (this.current && now >= this.current.until) { this.current = null;this.bump(); }
    this.pending = this.pending.filter(n => n.expiresAt > now);
    if (this.enabled && !this.current && this.pending.length && now >= this.nextAt) {
      const item = this.pending.shift(), duration = item.level === 'attention' ? 9000 : this.runtime.game ? 3500 : 5500;
      this.current = { ...item, until: now + duration };
      this.nextAt = now + duration + (this.runtime.game ? 6000 : 1500);this.bump();
    }
  }
  snapshot(state, services) {
    this.observe(state, services);const now = this.runtime.now();this.advance(now);
    return { enabled: this.enabled, sequence: this.sequence, waiting: this.pending.length,
      current: this.current ? { id: this.current.id, level: this.current.level, title: this.current.title, detail: this.current.detail,
        view: this.current.group === 'services-attention' ? 'components' : 'activity',
        count: this.current.count, reference: clone(this.current.reference), remainingMs: Math.max(0, Math.min(9000, this.current.until - now)) } : null };
  }
  dismiss({ id }) {
    invariant(typeof id === 'string' && /^[a-f0-9]{32}$/.test(id), 'NOTICE_ID', 'Choose the displayed notice.');
    this.observe();
    if (this.current?.id === id) { this.current = null;this.bump(); }
    this.runtime.sequence++;return { dismissed: true };
  }
}
