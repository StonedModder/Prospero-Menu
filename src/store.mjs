// SPDX-License-Identifier: GPL-3.0-or-later
import { clone, canonical, invariant, PmError } from './errors.mjs';
import { initialState, validateState } from './settings.mjs';

// Two independent snapshots: a torn write never replaces the sole good copy.
// Storage supplies durable atomic replace; checksum supplies corruption detection.
export class StateStore {
  constructor(storage, checksum) { this.storage = storage; this.checksum = checksum; this.tail = Promise.resolve(); }
  async open() {
    const copies = []; let present = 0;
    for (const slot of [0, 1]) {
      const text = await this.storage.read(`state-${slot}.json`);
      if (text == null) continue;
      present++;
      try {
        const envelope = JSON.parse(text);
        invariant(envelope.checksum === this.checksum(canonical(envelope.state)), 'STATE_CHECKSUM', 'Invalid snapshot checksum.');
        const state = validateState(envelope.state);
        invariant(state.revision % 2 === slot, 'STATE_SLOT', 'Invalid snapshot slot.');
        copies.push(state);
      } catch { /* The other independently committed slot can recover this one. */ }
    }
    if (present && !copies.length) throw new PmError('STATE_CORRUPT',
      'Neither saved state snapshot is readable.', { action: 'Preserve the state files for support; no settings were overwritten.' });
    this.state = copies.sort((a, b) => b.revision - a.revision)[0] || initialState();
    this.previousState = copies[1] || null;
    this.recovered = present > copies.length;
    return this;
  }
  snapshot() { return clone(this.state); }
  inspect(read) {
    const work = this.tail.then(() => read(this.snapshot()));
    this.tail = work.catch(() => {});
    return work;
  }
  transaction(change, expectedRevision) {
    const work = this.tail.then(async () => {
      invariant(this.state, 'STATE_NOT_OPEN', 'State is not open.');
      invariant(expectedRevision === undefined || expectedRevision === this.state.revision,
        'STATE_CHANGED', 'Settings changed on another device. Refresh and try again.');
      const next = clone(this.state);
      const result = await change(next);
      next.revision = this.state.revision + 1;
      validateState(next);
      const envelope = JSON.stringify({ state: next, checksum: this.checksum(canonical(next)) });
      invariant(new TextEncoder().encode(envelope).length <= 4 * 1024 * 1024, 'STATE_FULL', 'Saved state reached its size limit. Archive completed operations first.');
      await this.storage.replace(`state-${next.revision % 2}.json`, envelope);
      this.previousState = this.state;
      this.state = next;
      return { result, revision: next.revision };
    });
    this.tail = work.catch(() => {});
    return work;
  }
}
