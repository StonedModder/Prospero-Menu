// SPDX-License-Identifier: GPL-3.0-or-later
import { canonical, clone, invariant } from './errors.mjs';

// This is an evidence projection, never a dump of runtime state or native memory.
// Unknown fields, native prose, paths, request keys and credentials are omitted.
const BUILD = typeof PROSPEROMENU_BUILD === 'undefined' ? { source: 'development-unfingerprinted', components: [] } : PROSPEROMENU_BUILD;
const LIMIT = 512, RUNS = 3, MAX_BYTES = 2 * 1024 * 1024;
const token = (v, pattern = /^[a-zA-Z0-9_.-]{1,80}$/) => typeof v === 'string' && pattern.test(v) ? v : null;
const code = v => token(v, /^[A-Z][A-Z0-9_]{0,79}$/);
const number = v => Number.isSafeInteger(v) && Math.abs(v) <= Number.MAX_SAFE_INTEGER ? v : null;
const numeric = ['pid', 'appId', 'errno', 'error', 'status', 'result', 'returnCode', 'handle', 'startResult', 'code', 'stage', 'completed', 'total', 'completedBytes', 'totalBytes', 'bytes', 'count'];
const flags = ['ready', 'absent', 'busy', 'verified', 'completed', 'installed', 'initialized', 'adopted', 'injectionReady', 'noSideEffects', 'safe', 'halted', 'accepted', 'observed', 'mounted', 'sameBoot'];
export function diagnosticEvidence(value, depth = 0) {
  const out = {};
  if (!value || typeof value !== 'object' || depth > 2) return out;
  for (const key of numeric) if (number(value[key]) !== null) out[key] = value[key];
  for (const key of flags) if (typeof value[key] === 'boolean') out[key] = value[key];
  if (code(value.error?.code)) out.error = { code: code(value.error.code) };
  if (code(value.reasonCode)) out.reasonCode = code(value.reasonCode);
  // Only known receipt containers are traversed, never arbitrary nested values.
  for (const key of ['evidence', 'receipt', 'progress']) if (value[key] && depth < 2) out[key] = diagnosticEvidence(value[key], depth + 1);
  return out;
}
const buildProjection = value => ({ source: token(value?.source) || 'unknown', components: Array.isArray(value?.components) ?
  value.components.slice(0, 16).map(c => ({ id: token(c.id), version: token(c.version), sha256: token(c.sha256, /^[a-f0-9]{64}$/) })) : [] });
const gameProjection = value => value ? { titleId: token(value.titleId, /^(PPSA|CUSA)[0-9]{5}$/),
  version: token(value.version, /^[0-9.]{1,16}$/), pid: number(value.pid), appId: number(value.appId) } : null;
function eventProjection(e) {
  const out = { n: number(e.n), at: number(e.at), elapsedMs: number(e.elapsedMs), clockReversed: e.clockReversed === true,
    type: token(e.type), target: token(e.target), status: token(e.status), code: code(e.code),
    component: ['kstuff', 'shadowmount', 'payload-manager'].includes(e.component) ? e.component : null,
    origin: ['current-session', 'previous-session'].includes(e.origin) ? e.origin : null, baseline: e.baseline === true,
    evidence: diagnosticEvidence(e.evidence), game: gameProjection(e.game), evidenceTruncated: e.evidenceTruncated === true };
  if (canonical(out).length > 1024) { out.evidence = diagnosticEvidence(e.evidence, 2); out.evidenceTruncated = true; }
  if (canonical(out).length > 1024) {
    const summary = {};
    for (const key of ['stage', 'errno', 'error', 'ready', 'noSideEffects'])
      if (Object.hasOwn(out.evidence, key)) summary[key] = out.evidence[key];
    out.evidence = summary;
  }
  return out;
}
function runProjection(run) {
  invariant(run && Array.isArray(run.events) && run.events.length <= LIMIT && Number.isSafeInteger(run.sequence), 'DIAGNOSTIC_FORMAT', 'Invalid capture.');
  return { capture: token(run.capture), startedAt: number(run.startedAt), startupIncluded: run.startupIncluded === true,
    build: buildProjection(run.build), firmware: token(run.firmware, /^\d{2}\.\d{2}$/), mode: ['console', 'development'].includes(run.mode) ? run.mode : 'unknown',
    ended: ['disabled', 'coordinator-close', 'unconfirmed-stop', 'startup-failed', 'cleared'].includes(run.ended) ? run.ended : null,
    sequence: run.sequence, omitted: Math.max(0, number(run.omitted) || 0), events: run.events.map(eventProjection) };
}

export class SupportRecorder {
  constructor(runtime) {
    this.runtime = runtime; this.storage = runtime.store.storage; this.checksum = runtime.store.checksum;
    this.runs = []; this.current = null; this.revision = 0; this.writer = null; this.dirty = false;
    this.fault = null; this.recovered = false; this.opened = false; this.jobs = new Map(); this.services = new Map();
    this.queueState = null; this.lastGame = null; this.lastAt = 0;
  }
  async open() {
    let present = 0; const valid = [];
    for (const slot of [0, 1]) {
      try {
        const raw = await this.storage.read(`diagnostics-${slot}.json`);
        if (raw == null) continue;
        present++;
        const data = JSON.parse(raw), d = data.document;
        invariant(raw.length <= MAX_BYTES && d?.schema === 1 && Number.isSafeInteger(d.revision) && d.revision >= 0 &&
          d.revision % 2 === slot && Array.isArray(d.runs) && d.runs.length <= RUNS && data.checksum === this.checksum(canonical(d)),
        'DIAGNOSTIC_FORMAT', 'Invalid capture snapshot.');
        valid.push({ revision: d.revision, runs: d.runs.map(runProjection) });
      } catch { this.recovered = true; }
    }
    const saved = valid.sort((a, b) => b.revision - a.revision)[0];
    if (saved) { this.runs = saved.runs; this.revision = saved.revision; }
    if ((present || this.recovered) && !valid.length) this.fault = 'CAPTURE_CORRUPT';
    // Preserve unreadable evidence until the user explicitly clears it.
    this.opened = true;
    this.configure(this.runtime.store.snapshot().settings.developer.diagnostics, true);
    await this.flush();
  }
  configure(enabled, startup = false) {
    if (!this.opened) return;
    if (enabled && !this.current && this.fault !== 'CAPTURE_CORRUPT') {
      for (const run of this.runs) if (!run.ended) run.ended = 'unconfirmed-stop';
      this.jobs.clear(); this.services.clear(); this.queueState = null; this.lastGame = null;
      this.current = { capture: this.runtime.id(), startedAt: this.runtime.now(), startupIncluded: startup,
        build: buildProjection(BUILD), firmware: null, mode: 'unknown', ended: null, sequence: 0, omitted: 0, events: [] };
      this.runs.push(this.current); this.runs = this.runs.slice(-RUNS); this.lastAt = this.current.startedAt;
      this.record('capture', { status: startup ? 'startup' : 'enabled-mid-session' }); this.identity(); this.observe();
    } else if (!enabled && this.current) this.finish('disabled');
  }
  identity() {
    if (!this.current) return;
    this.current.firmware = token(this.runtime.identity?.firmware, /^\d{2}\.\d{2}$/);
    this.current.mode = ['console', 'development'].includes(this.runtime.identity?.mode) ? this.runtime.identity.mode : 'unknown';
    this.record('identity', { status: this.current.mode });
  }
  record(type, fields = {}) {
    if (!this.current || this.closed) return;
    const now = this.runtime.now(), run = this.current;
    run.events.push(eventProjection({ ...fields, type, n: ++run.sequence, at: now,
      elapsedMs: Math.max(0, now - run.startedAt), clockReversed: now < this.lastAt }));
    this.lastAt = now;
    if (run.events.length > LIMIT) { run.events.splice(64, 1); run.omitted++; }
    this.dirty = true; this.schedule();
  }
  observe() {
    if (!this.current) return;
    const state = this.runtime.store.snapshot();
    for (const job of state.jobs) {
      if (job.session !== this.runtime.session && ['completed', 'failed', 'cancelled'].includes(job.status)) continue;
      const previous = this.jobs.get(job.id), target = previous?.target || `job-${this.jobs.size + 1}`;
      const value = { status: token(job.status), code: code(job.error?.code), component: job.args?.component,
        evidence: diagnosticEvidence({ ...job.result, receipt: job.checkpoint, progress: job.progress }) };
      const signature = canonical(value);
      if (signature !== previous?.signature) {
        this.jobs.set(job.id, { ...previous, target, signature });
        this.record('job.' + (token(job.kind) || 'unknown'), { target, ...value, baseline: !previous,
          origin: job.session === this.runtime.session ? 'current-session' : 'previous-session' });
      }
    }
    for (const service of this.runtime.graph.snapshot()) {
      const value = { target: token(service.id), status: token(service.status), code: code(service.error?.code), evidence: diagnosticEvidence(service.evidence) };
      const signature = canonical(value);
      if (signature !== this.services.get(service.id)) { this.services.set(service.id, signature); this.record('service', value); }
    }
    const q = { halted: this.runtime.jobs.halted, safe: this.runtime.graph.safe }, queue = canonical(q);
    if (queue !== this.queueState) { this.queueState = queue; this.record('queue', { evidence: q, code: code(this.runtime.jobs.fault?.code) }); }
  }
  game(phase, value, error) {
    if (!this.current) return;
    const fields = { status: token(phase), game: gameProjection(value), code: code(error?.code) }, signature = canonical(fields);
    if (signature !== this.lastGame) { this.lastGame = signature; this.record('game', fields); }
  }
  receipt(job, value) {
    if (!this.current) return;
    const previous = this.jobs.get(job.id) || { target: `job-${this.jobs.size + 1}` };
    const evidence = diagnosticEvidence(value), receipt = canonical(evidence);
    if (previous.receipt === receipt) return;
    this.jobs.set(job.id, { ...previous, receipt });
    this.record('native.' + (token(job.kind) || 'unknown'), { target: previous.target, component: job.args?.component, evidence });
  }
  finish(reason) {
    if (!this.current) return;
    this.record('capture', { status: reason }); this.current.ended = reason; this.current = null; this.dirty = true; this.schedule();
  }
  schedule() {
    if (this.writer || this.fault || !this.dirty) return;
    // One active write and one coalesced successor: no unbounded event/file queue.
    this.writer = Promise.resolve().then(async () => {
      if (this.dirty && !this.fault) {
        this.dirty = false;
        const document = { schema: 1, revision: this.revision + 1, runs: clone(this.runs) };
        const text = JSON.stringify({ document, checksum: this.checksum(canonical(document)) });
        invariant(text.length <= MAX_BYTES, 'CAPTURE_LIMIT', 'Support capture exceeded its bound.');
        await this.storage.replace(`diagnostics-${document.revision % 2}.json`, text);
        this.revision = document.revision;
      }
    }).catch(() => { this.fault = 'CAPTURE_WRITE_FAILED'; this.dirty = true; })
      .finally(() => { this.writer = null; this.schedule(); });
  }
  async flush(retry = false) {
    await this.writer;
    if (retry && this.fault === 'CAPTURE_WRITE_FAILED') this.fault = null;
    this.schedule(); await this.writer;
  }
  snapshot(state = this.runtime.store.snapshot()) {
    return { enabled: Boolean(this.current), requested: state.settings.developer.diagnostics,
      captures: this.runs.length, events: this.runs.reduce((n, r) => n + r.events.length, 0),
      omitted: this.runs.reduce((n, r) => n + r.omitted, 0), recovered: this.recovered,
      fault: this.fault, pending: Boolean(this.writer || this.dirty), source: BUILD.source };
  }
  async export() {
    await this.flush(true);
    invariant(this.runs.length, 'CAPTURE_EMPTY', 'Enable support capture and reproduce the issue first.');
    return { schema: 1, product: 'ProsperoMenu', exportedAt: this.runtime.now(), persistence: this.snapshot(),
      scope: 'Opt-in coordinator events and selected native receipt fields. No raw memory, paths, credentials or uploaded content. No automatic upload.',
      limitations: 'Abrupt power loss can omit the last pending write. Unconfirmed stop is not proof of a crash. Coordinator close does not prove native jobs settled. Early startup before the coordinator and omitted ring events are not captured.',
      runs: this.runs.map(runProjection) };
  }
  async clear() {
    await this.flush();
    this.current = null; this.runs = []; this.jobs.clear(); this.services.clear(); this.fault = null; this.recovered = false;
    // Replace both slots so clearing cannot resurrect older evidence on recovery.
    this.dirty = true; await this.flush(true);
    invariant(!this.fault, 'CAPTURE_WRITE_FAILED', 'Could not clear saved captures.');
    this.dirty = true; await this.flush(true);
    invariant(!this.fault, 'CAPTURE_WRITE_FAILED', 'Could not clear both saved capture slots.');
    this.configure(this.runtime.store.snapshot().settings.developer.diagnostics);
    await this.flush(); return this.snapshot();
  }
  async close() { this.finish('coordinator-close'); this.closed = true; await this.flush(); }
}
