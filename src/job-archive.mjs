// SPDX-License-Identifier: GPL-3.0-or-later
import { canonical, clone, invariant } from './errors.mjs';

const terminal = new Set(['completed', 'failed', 'cancelled']);
// These records are not native residency/once-per-boot or reversible-state guards.
// Unknown kinds stay resident until their consumers explicitly support archival.
const eligible = new Set(['memory.inspect', 'save.refresh', 'save.export', 'package.install',
  'package.verify', 'package.download', 'package.receive', 'payload.dispatch', 'payload.startup', 'payload.download', 'library.storage', 'update.stage']);
const bytes = text => new TextEncoder().encode(text).length;
const file = (bucket, slot) => `jobs-${bucket}-${slot}.json`;

export function validateJobArchive(value = {}) {
  invariant(value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length <= 64,
    'STATE_INVALID', 'Saved operation archive is invalid.');
  for (const [bucket, ref] of Object.entries(value)) invariant(/^[0-9a-f]{2}$/.test(bucket) && parseInt(bucket, 16) < 64 &&
    ref && Number.isInteger(ref.slot) && ref.slot >= 0 && ref.slot < 3 && /^[a-f0-9]{64}$/.test(ref.hash) &&
    Number.isSafeInteger(ref.count) && ref.count > 0 && ref.count <= 8192,
  'STATE_INVALID', 'Saved operation archive reference is invalid.');
  return value;
}

export class JobArchive {
  constructor(store) { this.store = store; }
  bucket(key) { return (parseInt(this.store.checksum(key).slice(0, 2), 16) & 63).toString(16).padStart(2, '0'); }
  async read(state, bucket) {
    const ref = state.jobArchive?.[bucket];
    if (!ref) return [];
    const text = await this.store.storage.read(file(bucket, ref.slot));
    invariant(typeof text === 'string' && bytes(text) <= 4 * 1024 * 1024 && this.store.checksum(text) === ref.hash,
      'JOB_ARCHIVE_DAMAGED', 'Saved operation receipts are unavailable. No new work was accepted in this archive group.',
      { action: 'Preserve the ProsperoMenu state files for recovery; do not delete receipts to retry work.' });
    let document;
    try { document = JSON.parse(text); } catch { /* Reject below without replay. */ }
    invariant(document?.format === 'prosperomenu-jobs-v1' && document.bucket === bucket &&
      Array.isArray(document.jobs) && document.jobs.length === ref.count,
    'JOB_ARCHIVE_DAMAGED', 'Saved operation archive contents are invalid.');
    const keys = new Set();
    for (const job of document.jobs) {
      invariant(job && /^[a-f0-9]{32}$/.test(job.id) && typeof job.key === 'string' && /^[a-zA-Z0-9_-]{8,100}$/.test(job.key) &&
        this.bucket(job.key) === bucket && !keys.has(job.key) && terminal.has(job.status) &&
        typeof job.requestSignature === 'string' && typeof job.signature === 'string',
      'JOB_ARCHIVE_DAMAGED', 'Saved operation archive identity is invalid.');
      keys.add(job.key);
    }
    return document.jobs;
  }
  async find(state, key) {
    const job = (await this.read(state, this.bucket(key))).find(job => job.key === key);
    return job ? { ...clone(job), archived: true } : null;
  }
  async findId(state, id) {
    for (const bucket of Object.keys(state.jobArchive || {})) {
      const job = (await this.read(state, bucket)).find(job => job.id === id);
      if (job) return { ...clone(job), archived: true };
    }
    return null;
  }
  // Caller holds StateStore's serialized transaction throughout. A third slot
  // lets us preserve BOTH durable state snapshots, including after failed writes.
  async compact(state, busy) {
    if (state.jobs.length < 768 && bytes(JSON.stringify(state.jobs)) < 2 * 1024 * 1024) return 0;
    const recent = new Set(state.jobs.slice(-256).map(job => job.id));
    const candidates = state.jobs.filter(job => terminal.has(job.status) && eligible.has(job.kind) &&
      job.requestSignature && /^[a-f0-9]{32}$/.test(job.id) && !recent.has(job.id) && !busy.has(job.id));
    // Retain the latest successful package verification: the installer binds to it.
    const latest = new Map();
    for (const job of state.jobs) if (job.kind === 'package.verify' && job.status === 'completed') latest.set(job.args.id, job.id);
    for (const job of state.jobs) if (job.kind === 'package.download' && job.status === 'completed') latest.set(job.result.id, job.id);
    const protectedIds = new Set(latest.values());
    // Keep referenced child/undo receipts even if their own kind is archival-safe.
    const referenced = new Set();
    function strings(value, own) {
      if (typeof value === 'string') { if (value !== own.id && value !== own.key) referenced.add(value); }
      else if (value && typeof value === 'object') for (const child of Object.values(value)) strings(child, own);
    }
    for (const job of state.jobs) { strings(job.args, job); strings(job.checkpoint, job); strings(job.result, job); }
    const chosen = candidates.filter(job => !protectedIds.has(job.id) && !referenced.has(job.id) && !referenced.has(job.key)).slice(0, 64);
    const groups = new Map();
    for (const job of chosen) {
      const bucket = this.bucket(job.key);
      if (!groups.has(bucket)) groups.set(bucket, []);
      groups.get(bucket).push(job);
    }
    state.jobArchive ??= {};
    for (const [bucket, jobs] of groups) {
      const existing = await this.read(state, bucket);
      for (const job of jobs) invariant(!existing.some(prior => prior.key === job.key), 'JOB_ARCHIVE_CONFLICT', 'An operation already has a different archived record.');
      const document = { format: 'prosperomenu-jobs-v1', bucket, jobs: [...existing, ...jobs] };
      const text = canonical(document);
      invariant(document.jobs.length <= 8192 && bytes(text) <= 4 * 1024 * 1024,
        'JOB_ARCHIVE_FULL', 'This operation archive group reached its storage limit. Existing receipts were retained.');
      const reserved = [state.jobArchive[bucket]?.slot, this.store.previousState?.jobArchive?.[bucket]?.slot];
      const slot = [0, 1, 2].find(value => !reserved.includes(value));
      await this.store.storage.replace(file(bucket, slot), text);
      state.jobArchive[bucket] = { slot, hash: this.store.checksum(text), count: document.jobs.length };
    }
    const archived = new Set(chosen.map(job => job.id));
    state.jobs = state.jobs.filter(job => !archived.has(job.id));
    return chosen.length;
  }
}
