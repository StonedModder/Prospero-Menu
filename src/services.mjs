// SPDX-License-Identifier: GPL-3.0-or-later
import { clone, invariant, publicError, PmError } from './errors.mjs';

export class ServiceGraph {
  constructor({ timeout = 15000, changed = () => {}, allowOptional = () => false } = {}) {
    this.definitions = new Map(); this.states = new Map(); this.pending = new Map();
    this.timeout = timeout; this.changed = changed; this.generation = 0; this.safe = false;
    this.allowOptional = allowOptional;
  }
  register(definition) {
    invariant(/^[a-z][a-z0-9-]{0,40}$/.test(definition.id) && !this.definitions.has(definition.id),
      'SERVICE_ID', 'Service identifiers must be unique.');
    this.definitions.set(definition.id, { dependencies: [], conflicts: [], optional: false, ...definition });
    this.states.set(definition.id, { id: definition.id, status: 'stopped', evidence: null, error: null });
  }
  validate() {
    const visiting = new Set(), visited = new Set();
    const visit = id => {
      invariant(this.definitions.has(id), 'SERVICE_MISSING', `Missing service ${id}.`);
      invariant(!visiting.has(id), 'SERVICE_CYCLE', `Service dependency cycle at ${id}.`);
      if (visited.has(id)) return;
      visiting.add(id);
      for (const dep of this.definitions.get(id).dependencies) visit(dep);
      visiting.delete(id); visited.add(id);
    };
    for (const id of this.definitions.keys()) visit(id);
  }
  snapshot() { return [...this.states.values()].map(clone); }
  update(id, patch) { Object.assign(this.states.get(id), patch); this.changed(); }
  ready(id) { return this.states.get(id)?.status === 'ready'; }
  start(id) {
    if (this.pending.has(id)) return this.pending.get(id).visible;
    this.validate();
    const definition = this.definitions.get(id);
    invariant(definition, 'SERVICE_MISSING', `Unknown service ${id}.`);
    if (this.ready(id)) return Promise.resolve(clone(this.states.get(id)));
    invariant(!(this.safe && definition.optional && !this.allowOptional(id)), 'SAFE_START', 'Optional components are disabled for this startup.',
      { action: 'Review the previous startup failure before enabling optional components.' });
    invariant(!['failed', 'uncertain', 'blocked'].includes(this.states.get(id).status), 'SERVICE_RETRY_REQUIRED',
      'This service requires explicit reconciliation before retrying.');
    for (const [otherId, other] of this.definitions) {
      if (definition.conflicts.includes(otherId) || other.conflicts.includes(id)) {
        invariant(!this.pending.has(otherId) && !['ready', 'starting', 'uncertain'].includes(this.states.get(otherId).status),
          'SERVICE_CONFLICT', `${id} conflicts with ${otherId}.`);
      }
    }
    const controller = new AbortController(), token = ++this.generation;
    let resolve, reject;
    const visible = new Promise((yes, no) => { resolve = yes; reject = no; });
    const entry = { visible, controller, token, expired: false, reject };
    this.pending.set(id, entry);
    this.update(id, { status: 'starting', error: null, evidence: null });
    const timer = setTimeout(() => {
      entry.expired = true; controller.abort();
      const error = new PmError('SERVICE_TIMEOUT', `${id} did not confirm readiness.`,
        { action: 'Wait for reconciliation. Starting another copy may conflict with the first.' });
      this.update(id, { status: 'uncertain', error: error.toJSON() }); reject(error);
      // Keep pending until the real operation returns. A timeout is not cancellation.
    }, definition.timeout ?? this.timeout);
    entry.settled = (async () => {
      try {
        for (const dep of definition.dependencies) await this.start(dep);
        if (controller.signal.aborted) throw new PmError('SERVICE_TIMEOUT', 'Startup expired before execution.');
        const evidence = await definition.start({ signal: controller.signal, generation: token });
        invariant(!entry.expired, 'SERVICE_LATE_ACK', 'Readiness arrived after the startup deadline.');
        invariant(await definition.verify(evidence), 'SERVICE_UNVERIFIED', `${id} returned no valid readiness evidence.`);
        invariant(!entry.expired, 'SERVICE_LATE_ACK', 'Readiness verification exceeded its deadline.');
        this.update(id, { status: 'ready', evidence: clone(evidence), error: null });
        resolve(clone(this.states.get(id)));
      } catch (error) {
        if (!entry.expired) {
          this.update(id, { status: 'failed', error: publicError(error) }); reject(error);
        }
      } finally {
        clearTimeout(timer);
        if (this.pending.get(id) === entry) this.pending.delete(id);
      }
    })();
    return visible;
  }
  invalidate(id, reason) {
    invariant(this.states.has(id), 'SERVICE_MISSING', 'Unknown service.');
    const entry = this.pending.get(id);
    if (entry) { entry.expired = true; entry.controller.abort(); entry.reject(reason); }
    this.update(id, { status: 'failed', evidence: null, error: publicError(reason) });
    for (const [child, def] of this.definitions) {
      if (def.dependencies.includes(id) && this.states.get(child).status !== 'stopped')
        this.invalidate(child, new PmError('DEPENDENCY_LOST', `${child} lost dependency ${id}.`));
    }
  }
  async reconcile(id) {
    invariant(!this.pending.has(id), 'SERVICE_BUSY', 'The previous operation has not settled.');
    const definition = this.definitions.get(id);
    invariant(definition?.inspect, 'SERVICE_INSPECTION_UNAVAILABLE', 'This service cannot yet be reconciled automatically.');
    const evidence = await definition.inspect();
    if (evidence?.absent === true) {
      this.invalidate(id, new PmError('SERVICE_ABSENT', `${id} is no longer running.`));
      this.update(id, { status: 'stopped', evidence: null, error: null });
    }
    else {
      invariant(await definition.verify(evidence), 'SERVICE_UNVERIFIED', 'The existing service could not be verified.');
      this.update(id, { status: 'ready', evidence: clone(evidence), error: null });
    }
    return clone(this.states.get(id));
  }
}
