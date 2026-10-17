// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { canonical, clone, invariant, PmError } from './errors.mjs';
import { requireGame } from './firmware.mjs';
import { validateComparison, compareCandidate, candidateWindows } from './scan-refinement.mjs';
const hex = /^0x[0-9a-f]{1,16}$/;
const same = (a, b) => b && ['pid', 'generation', 'titleId', 'version', 'metadataHash'].every(k => a[k] === b[k]);
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const clean = (code, message) => Object.assign(new PmError(code, message), { noSideEffects: true });
export function inspectionPacket(offset, length) {
  const packet = Buffer.alloc(16), view = new DataView(packet.buffer, packet.byteOffset, 16);
  view.setBigUint64(0, BigInt(offset), true);view.setUint32(8, length, true);view.setUint32(12, 0x544d5231, true);return packet;
}
export function scanPattern(value) {
  invariant(typeof value === 'string' && /^(?:[a-fA-F0-9]{2}|\?\?){1,128}$/.test(value) && /[a-fA-F0-9]/.test(value),
    'SCAN_PATTERN', 'Use 1–128 hex bytes, with ?? for optional wildcard bytes.');
  return value.match(/../g).map(pair => pair === '??' ? null : parseInt(pair, 16));
}
export class MemoryService {
  constructor(runtime, native, { delay = ms => new Promise(resolve => setTimeout(resolve, ms)) } = {}) {
    this.runtime = runtime;this.native = native;this.delay = delay;this.results = new Map();
    if (typeof native.inspectStart === 'function' && typeof native.inspectPoll === 'function') runtime.jobs.register('memory.inspect', this.handler());
  }
  permitted(input) {
    invariant(this.runtime.store.snapshot().settings.developer.enabled && !this.runtime.graph.safe,
      'DEVELOPER_DISABLED', 'Enable developer tools for this session before memory inspection.');
    if (input?.developerSession !== undefined || input?.developerGrant !== undefined) invariant(
      input.developerSession === this.runtime.session && input.developerGrant && input.developerGrant === this.runtime.mcp?.grant &&
      this.runtime.mcp.expires > this.runtime.now(), 'DEVELOPER_DISABLED', 'This MCP access grant ended.');
  }
  prune() {
    for (const [id, entry] of this.results) if (entry.expires <= this.runtime.now()) this.results.delete(id);
    while (this.results.size > 8) this.results.delete(this.results.keys().next().value);
  }
  result(id) {
    this.permitted();this.prune();const entry = this.results.get(id);
    invariant(entry, 'MEMORY_RESULT_EXPIRED', 'The memory result is no longer in RAM. Submit a new inspection when ready.');
    invariant(same(entry.game, this.runtime.game), 'GAME_CHANGED', 'The result belongs to a previous game instance.');
    return clone(entry.value);
  }
  forget() { this.results.clear(); }
  source(id) {
    invariant(typeof id === 'string' && /^[a-f0-9]{32}$/.test(id), 'SCAN_SOURCE', 'Choose a completed scan result.');
    this.prune();const entry = this.results.get(id);
    invariant(entry?.baseline && ['scan', 'refine'].includes(entry.value.mode), 'SCAN_SOURCE_EXPIRED', 'The source scan is unavailable or expired. Start a new scan.');
    invariant(same(entry.game, this.runtime.game), 'GAME_CHANGED', 'The source scan belongs to a previous game instance.');
    return entry;
  }
  async validate(input) {
    this.permitted(input);requireGame(this.runtime.game);const game = clone(this.runtime.game);
    const refining = input?.mode === 'refine', source = refining ? this.source(input.source) : null;
    const module = source?.value.module ?? input?.module;
    invariant(input && ['module', 'read', 'scan', 'refine'].includes(input.mode) && typeof module === 'string' &&
      /^[\x21-\x7e]{1,127}$/.test(module) && !/[/\\]/.test(module), 'MEMORY_INPUT', 'Choose a module basename and inspection mode.');
    const offset = source?.value.offset ?? input.offset ?? '0x0', length = source?.value.length ?? input.length ?? 0;
    invariant(hex.test(offset) && Number.isSafeInteger(length) && length >= 0 &&
      (input.mode === 'module' ? offset === '0x0' && length === 0 : length > 0 && length <= (input.mode === 'scan' || refining ? 16 * 1024 * 1024 : 65536)),
      'MEMORY_BOUNDS', 'Module reads are limited to 64 KiB and scan ranges to 16 MiB.');
    invariant(BigInt(offset) + BigInt(length) <= 0xffffffffffffffffn, 'MEMORY_BOUNDS', 'Inspection range overflows the native address width.');
    let pattern = null, alignment = 1;
    if (input.mode === 'scan') {
      scanPattern(input.pattern);pattern = input.pattern.toLowerCase();alignment = input.alignment ?? 1;
      invariant([1, 2, 4, 8].includes(alignment), 'SCAN_ALIGNMENT', 'Choose alignment 1, 2, 4 or 8.');
    }
    if (refining) {
      validateComparison(input.comparison, input.dataType, source.value.width);alignment = source.value.alignment;
      if (input.comparison === 'equal' || input.comparison === 'different') {
        invariant(scanPattern(input.pattern).length === source.value.width, 'SCAN_PATTERN', 'The refinement pattern must match the original scan width.');pattern = input.pattern.toLowerCase();
      } else invariant(input.pattern == null, 'SCAN_PATTERN', 'This comparison uses the previous scan bytes, not a supplied pattern.');
    }
    const probe = this.native.systemProbe();
    invariant(!probe.busy && probe.kstuff?.injectionReady === true && /^[a-f0-9]{64}$/.test(game.metadataHash) &&
      /^\d+:\d+$/.test(probe.boot || '') && game.generation.startsWith(probe.boot + '/' + game.pid + '/'),
      'MEMORY_READINESS', 'Wait for verified kernel services and game metadata.');
    const accepted = { mode: input.mode, module, offset: '0x' + BigInt(offset).toString(16), length, pattern, alignment,
      pid: game.pid, generation: game.generation, titleId: game.titleId, version: game.version, metadataHash: game.metadataHash, boot: probe.boot };
    if (refining) Object.assign(accepted, { source: input.source, comparison: input.comparison, dataType: input.dataType ?? null });
    if (input.developerSession !== undefined) Object.assign(accepted, { developerSession: input.developerSession, developerGrant: input.developerGrant });
    if (input.generation) invariant(canonical(accepted) === canonical(input), 'GAME_CHANGED', 'The queued inspection belongs to another game context.');
    return accepted;
  }
  match(receipt, input, request, planHash) {
    invariant(receipt && receipt.request === request && receipt.planHash === planHash && receipt.pid === input.pid && receipt.generation === input.generation,
      'MEMORY_RECEIPT', 'The memory receipt does not match this game and request.');
  }
  async read(input, offset, length, context, index) {
    const request = hash(context.id + ':' + index).slice(0, 32), packet = inspectionPacket(offset, length), planHash = hash(packet);
    // Persist before the native attach. No checkpoint writes while it is stopped.
    await context.checkpoint({ request, planHash });
    for (;;) {
      try { this.permitted(input); } catch (error) { error.noSideEffects = true;throw error; }
      if (context.signal.aborted || !same(input, this.runtime.game) || !this.runtime.store.snapshot().settings.developer.enabled)
        throw clean('MEMORY_CANCELLED', 'Inspection stopped before the next native read.');
      try {
        this.native.inspectStart(request, input.pid, input.generation, input.titleId, input.metadataHash, input.module,
          packet.buffer.slice(packet.byteOffset, packet.byteOffset + packet.byteLength));break;
      } catch (error) { if (error.code !== 'NATIVE_BUSY') throw error;await this.delay(100); }
    }
    for (;;) {
      const receipt = this.native.inspectPoll(request);this.match(receipt, input, request, planHash);
      if (receipt.stage === 3 && receipt.resumed === true && receipt.restored === true && receipt.writes === 0) {
        invariant(receipt.bytes instanceof ArrayBuffer && receipt.bytes.byteLength === length && hex.test(receipt.base) && hex.test(receipt.size),
          'MEMORY_DATA_LOST', 'Resume was confirmed but the requested memory bytes are unavailable.');
        return { bytes: Buffer.from(receipt.bytes), base: receipt.base, size: receipt.size };
      }
      if (receipt.stage === 4 && receipt.resumed === true && receipt.restored === true) throw clean('MEMORY_REJECTED', receipt.message);
      invariant([1, 2].includes(receipt.stage) && !context.signal.aborted, 'MEMORY_UNCERTAIN', 'The stopped-game inspection did not confirm resume. Its attempt is held.');
      await this.delay(50);
    }
  }
  ended(input) {
    const probe = this.native.systemProbe();
    if (probe.busy || !/^\d+:\d+$/.test(probe.boot || '') || !Array.isArray(probe.processes)) return false;
    const process = probe.processes.find(p => p.pid === input.pid);
    return probe.boot !== input.boot || !process || (!!process.generation && process.generation !== input.generation);
  }
  async refine(input, context) {
    const source = this.source(input.source), previous = source.value, width = previous.width;
    const pattern = input.pattern ? scanPattern(input.pattern) : null, matches = [], samples = [];
    const current = () => {
      try { this.permitted(input);invariant(this.source(input.source) === source, 'SCAN_SOURCE_EXPIRED', 'The source scan ended during refinement. Partial results were discarded.'); }
      catch (error) { error.noSideEffects = true;throw error; }
    };
    let bytesRead = 0, index = 0;
    for (const window of candidateWindows(previous.matches, width)) {
      current();
      const part = await this.read(input, window.offset, window.length, context, index++);
      if (part.base !== previous.base || part.size !== previous.size) throw clean('MODULE_CHANGED', 'The source module changed. Start a new scan.');
      for (let i = window.first; i < window.last; i++) {
        const offset = Number(BigInt(previous.matches[i]) - window.offset), after = part.bytes.subarray(offset, offset + width);
        const before = source.baseline.subarray(i * width, (i + 1) * width);
        if (compareCandidate(before, after, input.comparison, pattern, input.dataType)) { matches.push(previous.matches[i]);samples.push(Buffer.from(after)); }
      }
      bytesRead += window.length;
      await context.progress({ completed: window.last, total: previous.matches.length, phase: 'Refining ' + input.module });
      if (context.signal.aborted) return { cancelled: true, message: 'Refinement cancelled after a confirmed game resume.' };
      if (window.last < previous.matches.length) await this.delay(50);
    }
    current();
    const value = { mode: 'refine', module: input.module, base: previous.base, size: previous.size, offset: previous.offset, length: previous.length,
      bytesRead, width, pattern: input.pattern, alignment: previous.alignment, matches, truncated: previous.truncated, atomicSnapshot: false,
      source: input.source, comparison: input.comparison, dataType: input.dataType, candidatesChecked: previous.matches.length };
    this.results.set(context.id, { game: clone(this.runtime.game), value, baseline: Buffer.concat(samples), expires: this.runtime.now() + 300000 });this.prune();
    return { completed: true, phase: 'inspected', generation: input.generation, pid: input.pid, bytesRead, truncated: previous.truncated, result: context.id,
      message: 'Refined retained candidates only. Bytes remain in session RAM for five minutes; reads are not one atomic snapshot.' };
  }
  handler() {
    const self = this, { runtime, native } = this;
    return { validate: input => self.validate(input), resources: input => ['native-payload-dispatch', 'game-memory:' + input.generation],
      timeout: 600000, cancellable: true,
      async run(input, context) {
        try { await self.validate(input); } catch (e) { e.noSideEffects = true;throw e; }
        if (input.mode === 'refine') {
          try { return await self.refine(input, context); }
          catch (error) {
            if (context.signal.aborted && error.noSideEffects) return { cancelled: true, message: 'Refinement cancelled with no native read left running.' };
            throw error;
          }
        }
        const pattern = input.mode === 'scan' ? scanPattern(input.pattern) : null, matches = [], samples = [];
        let done = 0, carry = Buffer.alloc(0), base, size, value, truncated = false, index = 0;
        do {
          const count = Math.min(65536, input.length - done), offset = BigInt(input.offset) + BigInt(done);
          let part;
          try { part = await self.read(input, offset, count, context, index++); }
          catch (error) {
            if (context.signal.aborted && error.noSideEffects) return { cancelled: true, message: 'Inspection cancelled with no native read left running.' };
            throw error;
          }
          if (base && (base !== part.base || size !== part.size)) throw clean('MODULE_CHANGED', 'The module changed during inspection. Discarding partial results.');
          base = part.base;size = part.size;
          if (!pattern) value = { mode: input.mode, module: input.module, base, size, offset: input.offset, bytes: part.bytes.toString('hex') };
          else {
            const bytes = Buffer.concat([carry, part.bytes]), start = offset - BigInt(carry.length);
            for (let at = 0; at <= bytes.length - pattern.length; at++) {
              const where = start + BigInt(at);
              if (where % BigInt(input.alignment) || !pattern.every((v, p) => v === null || bytes[at + p] === v)) continue;
              if (matches.length === 8192) { truncated = true;break; }
              matches.push('0x' + where.toString(16));
              samples.push(Buffer.from(bytes.subarray(at, at + pattern.length)));
            }
            carry = bytes.subarray(Math.max(0, bytes.length - pattern.length + 1));
          }
          done += count;
          await context.progress({ completed: done, total: input.length, phase: 'Inspecting ' + input.module });
          if (truncated || done === input.length) break;
          if (context.signal.aborted) return { cancelled: true, message: 'Scan cancelled between reads; game resume was confirmed.' };
          if (!runtime.store.snapshot().settings.developer.enabled) throw clean('MEMORY_CANCELLED', 'Scan stopped between reads; game resume was confirmed.');
          await self.delay(50);
        } while (done < input.length);
        invariant(same(input, runtime.game), 'GAME_CHANGED', 'The game changed before delivering memory results.');
        self.permitted(input);
        value ??= { mode: 'scan', module: input.module, base, size, offset: input.offset, length: input.length,
          bytesRead: done, width: pattern.length, pattern: input.pattern, alignment: input.alignment, matches, truncated, atomicSnapshot: false };
        self.results.set(context.id, { game: clone(runtime.game), value, ...(pattern ? { baseline: Buffer.concat(samples) } : {}), expires: runtime.now() + 300000 });self.prune();
        return { completed: true, phase: 'inspected', generation: input.generation, pid: input.pid, bytesRead: done, truncated, result: context.id,
          message: 'Memory result available in this session for five minutes. Scan chunks are read at different times.' };
      },
      verify: async (result, input) => result.completed && result.generation === input.generation && result.pid === input.pid && ['inspected', 'result-lost', 'game-ended'].includes(result.phase),
      async reconcile(job) {
        const cp = job.checkpoint;
        if (cp) {
          const receipt = native.inspectPoll(cp.request);
          if (receipt) {
            self.match(receipt, job.args, cp.request, cp.planHash);
            if (receipt.busy) return null;
            if ([3, 4].includes(receipt.stage) && receipt.resumed === true && receipt.restored === true && receipt.writes === 0)
              return { completed: true, phase: 'result-lost', generation: job.args.generation, pid: job.args.pid,
                message: 'Game resume was confirmed. The interrupted result was discarded; no read was replayed.' };
          }
        }
        if (self.ended(job.args)) return { completed: true, phase: 'game-ended', generation: job.args.generation, pid: job.args.pid,
          message: 'The target game instance ended. No memory read was replayed.' };
        return null;
      }
    };
  }
}
export function installMemory(runtime, native, options) { return runtime.memory = new MemoryService(runtime, native, options); }
