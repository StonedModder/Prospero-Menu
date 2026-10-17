// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { CheatPacks } from './cheat-packs.mjs';
import { cheatPacket } from './cheats.mjs';
import { requireGame } from './firmware.mjs';
import { canonical, clone, invariant, PmError } from './errors.mjs';
const same = (a, b) => b && ['pid', 'generation', 'titleId', 'version', 'metadataHash'].every(key => a[key] === b[key]);
const packetHash = packet => createHash('sha256').update(packet).digest('hex');
const clean = (code, message) => Object.assign(new PmError(code, message), { noSideEffects: true });
export class CheatService {
  constructor(runtime, native, storage, { delay = ms => new Promise(resolve => setTimeout(resolve, ms)) } = {}) {
    this.runtime = runtime; this.native = native; this.delay = delay; this.packs = new CheatPacks(runtime, storage);
    if (typeof native?.cheatStart === 'function' && typeof native?.cheatPoll === 'function') {
      runtime.jobs.register('cheat.set', this.handler());
      runtime.capability('cheats', true, 'Exact-version JSON, SHN/XML and MC4 cheats with native before-byte checks and rollback. Console validation pending.');
    }
    runtime.gameLifecycle?.subscribe(() => this.reconcileEnded());
  }
  list() { return this.packs.list(); }
  async inspect(sha256) {
    const pack = await this.packs.read(sha256), game = this.runtime.game, jobs = this.runtime.jobs.snapshot();
    return { sha256, titleId: pack.titleId, version: pack.version, name: pack.name, process: pack.process, credits: pack.credits,
      matches: !!game && game.titleId === pack.titleId && game.version === pack.version,
      cheats: pack.cheats.map(cheat => {
        const latest = jobs.filter(job => job.kind === 'cheat.set' && job.args.sha256 === sha256 && job.args.cheatId === cheat.id && job.args.generation === game?.generation).at(-1);
        return { id: cheat.id, name: cheat.name, description: cheat.description, supported: cheat.memory.every(p => p.absolute || p.section === 0),
          exactUndo: cheat.memory.every(p => p.on.length === p.off.length),
          lastApplied: latest?.status === 'completed' && latest.result?.phase === 'applied' ? latest.args.enabled : null,
          operation: latest ? { id: latest.id, status: latest.status, message: latest.error?.message || latest.result?.message } : null };
      }) };
  }
  outcome(receipt, input, id) {
    invariant(receipt && receipt.request === id && receipt.planHash === input.planHash && receipt.pid === input.pid && receipt.generation === input.generation,
      'CHEAT_RECEIPT', 'The native result does not match this cheat and game instance.');
    if (receipt.stage !== 3) return null;
    invariant(receipt.resumed === true && receipt.restored === false && Number.isInteger(receipt.writes) && receipt.writes > 0,
      'CHEAT_RECEIPT', 'The native patch result did not confirm applied bytes and game resume.');
    return { completed: true, phase: 'applied', request: id, planHash: input.planHash, generation: input.generation, pid: input.pid,
      enabled: input.enabled, message: receipt.message };
  }
  ended(input, id) {
    const probe = this.native.systemProbe();
    if (probe.busy || !/^\d+:\d+$/.test(probe.boot || '') || !Array.isArray(probe.processes)) return null;
    const process = probe.processes.find(item => item.pid === input.pid);
    if (probe.boot === input.boot && process && (!process.generation || process.generation === input.generation)) return null;
    return { completed: true, phase: 'game-ended', request: id, planHash: input.planHash, generation: input.generation, pid: input.pid,
      message: 'The target game instance ended. Its patch outcome was not confirmed, and no retry was issued.' };
  }
  async reconcileEnded() {
    for (const job of this.runtime.jobs.snapshot().filter(j => j.kind === 'cheat.set' && j.status === 'interrupted')) {
      try { if (this.ended(job.args, job.id)) await this.runtime.jobs.reconcile(job.id); } catch { /* Preserve unresolved ownership. */ }
    }
  }
  handler() {
    const self = this, { runtime, native } = this;
    function developerGate(input) {
      if (input.developerSession !== undefined || input.developerGrant !== undefined) invariant(
        input.developerSession === runtime.session && input.developerGrant && input.developerGrant === runtime.mcp?.grant &&
        runtime.mcp.expires > runtime.now() && runtime.store.snapshot().settings.developer.enabled && runtime.store.snapshot().settings.developer.memoryWrites,
        'MEMORY_WRITES_DISABLED', 'The MCP memory-write authorization ended. No new patch was started.');
    }
    async function validate(input, { jobId = null } = {}) {
      invariant(input && /^[a-f0-9]{64}$/.test(input.sha256) && typeof input.cheatId === 'string' && typeof input.enabled === 'boolean',
        'CHEAT_INPUT', 'Choose an imported cheat and its desired state.');
      invariant(!runtime.graph.safe, 'SAFE_STARTUP', 'Memory modifications are held during safe startup.');
      developerGate(input);
      const game = clone(runtime.game); requireGame(game);
      invariant(/^[a-f0-9]{64}$/.test(game.metadataHash), 'CHEAT_GAME', 'Wait for verified running-game metadata.');
      const pack = await self.packs.read(input.sha256);
      invariant(pack.titleId === game.titleId && pack.version === game.version && same(game, runtime.game), 'CHEAT_MISMATCH', 'Open the exact game and version targeted by this cheat.');
      const packet = cheatPacket(pack, input.cheatId, input.enabled), probe = native.systemProbe();
      invariant(!probe.busy && probe.kstuff?.injectionReady === true && typeof probe.boot === 'string' &&
        game.generation.startsWith(probe.boot + '/' + game.pid + '/'), 'CHEAT_READINESS', 'Wait for verified kernel services and game identity.');
      const accepted = { sha256: input.sha256, cheatId: input.cheatId, enabled: input.enabled, module: pack.process, planHash: packetHash(packet),
        boot: probe.boot, pid: game.pid, generation: game.generation, titleId: game.titleId, version: game.version, metadataHash: game.metadataHash };
      if (packet.readUInt32LE(4) === 3) accepted.exactUndo = false;
      if (input.developerSession !== undefined) Object.assign(accepted, { developerSession: input.developerSession, developerGrant: input.developerGrant });
      if (input.undoOf !== undefined) accepted.undoOf = clone(input.undoOf);
      runtime.changes.guardJobUndo(accepted, 'cheat.set', { jobId });
      if (input.generation) invariant(canonical(accepted) === canonical(input), 'CHEAT_CONTEXT_CHANGED', 'The queued cheat belongs to a different game context.');
      return accepted;
    }
    return { validate, guard: (input, state) => runtime.changes.guardJobUndo(input, 'cheat.set', { state }),
      resources: input => ['native-payload-dispatch', 'game-memory:' + input.generation], timeout: 30000, cancellable: false,
      async run(input, context) {
        let packet;
        try { await validate(input, { jobId: context.id }); packet = cheatPacket(await self.packs.read(input.sha256), input.cheatId, input.enabled);
          invariant(packetHash(packet) === input.planHash, 'CHEAT_CHANGED', 'Stored cheat bytes changed.');
        } catch (error) { error.noSideEffects = true; throw error; }
        for (;;) {
          if (context.signal.aborted || !same(input, runtime.game)) throw clean('CHEAT_CONTEXT_CHANGED', 'The game changed before cheat dispatch.');
          if (runtime.graph.safe) throw clean('SAFE_STARTUP', 'Memory modifications are held during safe startup.');
          try { developerGate(input);runtime.changes.guardJobUndo(input, 'cheat.set', { jobId: context.id }); }
          catch (error) { error.noSideEffects = true;throw error; }
          try {
            native.cheatStart(context.id, input.pid, input.generation, input.titleId, input.metadataHash, input.module,
              packet.buffer.slice(packet.byteOffset, packet.byteOffset + packet.byteLength)); break;
          } catch (error) { if (error.code !== 'NATIVE_BUSY') throw error; await self.delay(100); }
        }
        for (;;) {
          const receipt = native.cheatPoll(context.id); context.evidence?.(receipt);
          const result = self.outcome(receipt, input, context.id);
          if (result) return result;
          if (receipt.stage === 4 && receipt.restored === true && receipt.resumed === true) throw clean('CHEAT_REJECTED', receipt.message);
          invariant([1, 2].includes(receipt.stage), 'CHEAT_UNCERTAIN', receipt.message || 'The native cheat result is uncertain.');
          invariant(!context.signal.aborted, 'CHEAT_TIMEOUT', 'The cheat did not confirm completion. Its original attempt is held; no retry was issued.');
          await context.progress({ completed: 0, total: 1, phase: receipt.message }); await self.delay(100);
        }
      },
      verify: async (result, input) => result.completed && result.planHash === input.planHash && result.generation === input.generation &&
        result.pid === input.pid && ['applied', 'game-ended'].includes(result.phase),
      async reconcile(job) {
        const receipt = native.cheatPoll(job.id);
        if (receipt) {
          const result = self.outcome(receipt, job.args, job.id); if (result) return result;
          if (receipt.stage === 4 && receipt.restored === true && receipt.resumed === true) return { absent: true };
          if (receipt.busy) return null;
        }
        return self.ended(job.args, job.id);
      }
    };
  }
}
export function installCheats(runtime, native, storage, options) {
  return runtime.cheats = new CheatService(runtime, native, storage, options);
}
