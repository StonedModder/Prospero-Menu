// SPDX-License-Identifier: GPL-3.0-or-later
import { requireGame } from './firmware.mjs';
import { invariant, PmError } from './errors.mjs';
const same = (a, b) => b && ['pid', 'generation', 'titleId', 'version', 'metadataHash'].every(k => a[k] === b[k]);
const clean = (code, message) => Object.assign(new PmError(code, message), { noSideEffects: true });
export class GameInput {
  constructor(runtime, native, { delay = ms => new Promise(resolve => setTimeout(resolve, ms)), automatic = true } = {}) {
    this.runtime = runtime;this.native = native;this.delay = delay;this.pending = null;this.stopped = true;this.timer = null;this.observation = null;
    this.automatic = automatic;
    runtime.jobs.register('game.input.prepare', this.handler());
  }
  target(input) {
    const runtime = this.runtime, game = requireGame(runtime.game);
    invariant(!runtime.graph.safe, 'SAFE_STARTUP', 'Game input preparation is held during safe startup.');
    invariant(runtime.capabilities.get('deck').available && runtime.nativeDeck?.inspect().ready,
      'INPUT_RENDERER', 'Waiting for a verified native deck renderer.');
    invariant(/^[a-f0-9]{64}$/.test(game.metadataHash || ''), 'INPUT_METADATA', 'The running game metadata is not verified.');
    if (input?.generation) invariant(same(input, game), 'INPUT_GAME_CHANGED', 'The game changed before input preparation.');
    const probe = this.native.systemProbe();
    invariant(!probe.busy && probe.kstuff?.injectionReady === true && typeof probe.boot === 'string' &&
      game.generation.startsWith(probe.boot + '/' + game.pid + '/'), 'INPUT_READINESS', 'Waiting for verified kernel services and game identity.');
    return { pid: game.pid, generation: game.generation, titleId: game.titleId, version: game.version, metadataHash: game.metadataHash, boot: probe.boot };
  }
  outcome(receipt, input, request) {
    if (!receipt) return null;
    invariant(receipt.request === request && receipt.pid === input.pid && receipt.generation === input.generation && receipt.metadataHash === input.metadataHash,
      'INPUT_RECEIPT', 'Game input receipt does not match the accepted game instance.');
    if (receipt.stage === 2 && receipt.phase === 2)
      return { completed: true, phase: 'prepared', pid: input.pid, generation: input.generation, metadataHash: input.metadataHash,
        message: 'Shared game callback prepared. Renderer input capture is a separate readiness check.' };
    return null;
  }
  ended(input) {
    const probe = this.native.systemProbe();
    if (probe.busy || typeof probe.boot !== 'string' || !/^\d+:\d+$/.test(probe.boot) || !Array.isArray(probe.processes)) return null;
    const process = probe.processes.find(p => p.pid === input.pid);
    if (probe.boot === input.boot && process && (!process.generation || process.generation === input.generation)) return null;
    return { completed: true, phase: 'game-ended', pid: input.pid, generation: input.generation,
      message: 'The original game instance ended. No input hook was replayed.' };
  }
  handler() {
    const self = this, { runtime, native } = this;
    return {
      validate: input => self.target(input),
      resources: input => ['native-payload-dispatch', 'game-modules:' + input.generation, 'game-memory:' + input.generation], timeout: 30000, cancellable: false,
      async run(input, context) {
        for (;;) {
          if (context.signal.aborted || !same(input, runtime.game) || runtime.graph.safe || !runtime.capabilities.get('deck').available)
            throw clean('INPUT_CONTEXT_CHANGED', 'The game, renderer or startup state changed before input preparation.');
          try { native.gameInputStart(context.id, input.pid, input.generation, input.titleId, input.metadataHash);break; }
          catch (error) { if (error.code !== 'NATIVE_BUSY') throw error;await self.delay(100); }
        }
        for (;;) {
          const receipt = native.gameInputPoll(context.id, input.pid, input.generation); context.evidence?.(receipt);
          const result = self.outcome(receipt, input, context.id);
          if (result) return result;
          if (receipt?.stage === 3) throw clean('INPUT_REJECTED', receipt.message);
          invariant(receipt?.stage === 1, 'INPUT_UNCERTAIN', receipt?.message || 'Game input preparation is unconfirmed; no retry was issued.');
          invariant(!context.signal.aborted, 'INPUT_TIMEOUT', 'Input preparation remains recorded and requires reconciliation.');
          await self.delay(100);
        }
      },
      verify: (result, input) => result.completed && result.pid === input.pid && result.generation === input.generation && ['prepared', 'game-ended'].includes(result.phase),
      reconcile(job) {
        const receipt = native.gameInputPoll(job.id, job.args.pid, job.args.generation), result = self.outcome(receipt, job.args, job.id);
        if (result) return result;
        if (receipt?.stage === 3) return { absent: true };
        if (receipt?.busy) return null;
        return self.ended(job.args);
      }
    };
  }
  async inspect() {
    const runtime = this.runtime;
    // An uncertain callback owns the shared native dispatch resource. Its
    // ownership ends with the verified process, even while no game is open.
    // Never require users to manually reconcile an exited game's operation.
    for (const job of runtime.jobs.snapshot().filter(job => job.kind === 'game.input.prepare' && job.status === 'interrupted')) {
      if (this.ended(job.args)) {
        try { await runtime.jobs.reconcile(job.id); } catch { /* Keep ownership if its outcome is still ambiguous. */ }
      }
    }
    const game = runtime.game;
    if (!game || runtime.graph.safe || runtime.recovery?.pending || !runtime.capabilities.get('deck').available) { this.observation = null;return; }
    const prior = runtime.jobs.snapshot().filter(job => job.kind === 'game.input.prepare' && job.args.generation === game.generation).at(-1);
    if (prior) {
      const receipt = this.native.gameInputPoll(prior.id, game.pid, game.generation);
      const prepared = this.outcome(receipt, prior.args, prior.id);
      this.observation = { generation: game.generation, phase: prepared ? 'prepared' : receipt?.stage === 4 ? 'uncertain' : receipt?.stage === 3 ? 'rejected' : 'unavailable', captureAvailable: false,
        message: prior.error?.message || receipt?.message || 'Game input preparation is recorded in Activity.' };
      return; // Existing requests, including failures, are never automatically replayed.
    }
    if (!this.automatic) {
      this.observation = { generation: game.generation, phase: 'not-prepared', captureAvailable: false,
        message: 'Automatic in-game menu preparation is disabled in this loader test build.' };
      return;
    }
    if (runtime.jobs.running.size) return;
    await runtime.jobs.enqueue('game.input.prepare', this.target(), runtime.id());
    this.observation = { generation: game.generation, phase: 'queued', captureAvailable: false, message: 'Preparing the shared game callback.' };
  }
  refresh() {
    if (this.pending) return this.pending;
    this.pending = this.inspect().catch(error => {
      this.observation = { generation: this.runtime.game?.generation, phase: 'unavailable', captureAvailable: false, message: error.message };
    }).finally(() => { this.pending = null; });return this.pending;
  }
  start() {
    if (!this.stopped) return;this.stopped = false;
    this.unsubscribe = this.runtime.gameLifecycle?.subscribe(event => {
      if (!['start','exit','available'].includes(event.type) || this.stopped) return;
      return this.pending ? this.pending.then(() => { if (!this.stopped) return this.refresh(); }) : this.refresh();
    });
    const run = async () => { if (this.stopped) return;await this.refresh();if (!this.stopped) this.timer = setTimeout(run, 2000); };run();
  }
  stop() { this.stopped = true;clearTimeout(this.timer);this.timer = null;this.unsubscribe?.();this.unsubscribe=null; }
}
export function installGameInput(runtime, native, options) {
  if (typeof native.gameInputStart !== 'function' || typeof native.gameInputPoll !== 'function') return null;
  return runtime.gameInput = new GameInput(runtime, native, options);
}
