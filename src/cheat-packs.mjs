// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { importCheatJson } from './cheats.mjs';
import { importCheatFile } from './cheat-import.mjs';
import { canonical, clone, invariant } from './errors.mjs';
const hashPattern = /^[a-f0-9]{64}$/;
export function validateCheatRegistry(entries) {
  invariant(Array.isArray(entries) && entries.length <= 256, 'CHEAT_REGISTRY', 'Invalid cheat registry.');
  const seen = new Set();
  return entries.map(entry => {
    invariant(entry && hashPattern.test(entry.sha256) && !seen.has(entry.sha256) &&
      /^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/.test(entry.titleId) &&
      /^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/.test(entry.version) &&
      typeof entry.name === 'string' && entry.name.length > 0 && entry.name.length <= 200 &&
      Number.isInteger(entry.count) && entry.count > 0 && entry.count <= 1024,
    'CHEAT_REGISTRY', 'Invalid cheat pack entry.');
    seen.add(entry.sha256);
    return { sha256: entry.sha256, titleId: entry.titleId, version: entry.version, name: entry.name, count: entry.count };
  });
}
const hash = text => createHash('sha256').update(text).digest('hex');
export class CheatPacks {
  constructor(runtime, storage) { this.runtime = runtime; this.storage = storage; this.tail = Promise.resolve(); }
  list() { return clone(this.runtime.store.snapshot().cheats || []); }
  import({ source, titleId, version, fallback }) {
    const work = this.tail.then(async () => {
      const pack = await importCheatFile(source, { titleId, version, fallback });
      titleId = pack.titleId; version = pack.version;
      const text = canonical({ id: titleId, version, name: pack.name, process: pack.process, credits: pack.credits,
        mods: pack.cheats.map(cheat => ({ name: cheat.name, description: cheat.description, memory: cheat.memory })) });
      invariant(Buffer.byteLength(text) <= 4 * 1024 * 1024, 'CHEAT_SIZE', 'The normalized cheat pack is too large.');
      const sha256 = hash(text), entry = { sha256, titleId, version, name: pack.name, count: pack.cheats.length };
      if (!this.list().some(item => item.sha256 === sha256)) {
        invariant(this.list().length < 256, 'CHEAT_REGISTRY_FULL', 'The cheat catalog has reached its 256-pack limit.');
        await this.storage.replace(sha256 + '.json', text);
        invariant(hash(await this.storage.read(sha256 + '.json')) === sha256, 'CHEAT_STORAGE', 'Cheat pack storage verification failed.');
        await this.runtime.store.transaction(state => { state.cheats ??= []; state.cheats.push(entry); });
        this.runtime.sequence++;
      }
      return entry;
    });
    this.tail = work.catch(() => {}); return work;
  }
  async read(sha256) {
    invariant(hashPattern.test(sha256), 'CHEAT_PACK', 'Choose an imported cheat pack.');
    const entry = this.list().find(item => item.sha256 === sha256);
    invariant(entry, 'CHEAT_PACK', 'Cheat pack is not registered.');
    const text = await this.storage.read(sha256 + '.json');
    invariant(typeof text === 'string' && hash(text) === sha256, 'CHEAT_STORAGE', 'Cheat pack is missing or changed on disk. Import it again from its source.');
    const pack = importCheatJson(text, entry);
    invariant(pack.name === entry.name && pack.cheats.length === entry.count, 'CHEAT_STORAGE', 'Cheat catalog and stored pack disagree.');
    return pack;
  }
}
