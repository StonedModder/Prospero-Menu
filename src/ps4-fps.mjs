// SPDX-License-Identifier: GPL-3.0-or-later
import { requireGame } from './firmware.mjs';
import { invariant, PmError } from './errors.mjs';
const isPs4 = game => /^(CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/.test(game?.titleId || '');
const same = (a, b) => b && ['pid', 'generation', 'titleId', 'version', 'metadataHash'].every(k => a[k] === b[k]);
const clean = (code, message) => Object.assign(new PmError(code, message), { noSideEffects: true });
const enabled = runtime => (runtime.overlaySettings?.() || runtime.store.snapshot().settings.overlay).fps;
export class Ps4Fps {
  constructor(runtime, native, { delay = ms => new Promise(resolve => setTimeout(resolve, ms)) } = {}) {
    this.runtime = runtime;this.native = native;this.delay = delay;this.pending = null;this.timer = null;this.stopped = true;
    this.observation = null;runtime.jobs.register('fps.ps4.prepare', this.handler());
  }
  target(input) {
    const runtime = this.runtime, game = requireGame(runtime.game);
    invariant(isPs4(game), 'FPS_GAME', 'PS4 frame counting requires a verified PS4 game.');
    invariant(/^[a-f0-9]{64}$/.test(game.metadataHash || ''), 'FPS_METADATA', 'The mounted game version has not been verified.');
    invariant(!runtime.graph.safe && enabled(runtime), 'FPS_DISABLED', 'FPS preparation is disabled or held during safe startup.');
    if (input?.generation) invariant(same(game, input), 'FPS_GAME_CHANGED', 'The game changed before FPS preparation.');
    const probe = this.native.systemProbe();
    invariant(!probe.busy && probe.kstuff?.injectionReady === true && typeof probe.boot === 'string' &&
      game.generation.startsWith(probe.boot + '/' + game.pid + '/'), 'FPS_READINESS', 'Waiting for verified kernel services before PS4 frame counting.');
    return { pid: game.pid, generation: game.generation, titleId: game.titleId, version: game.version, metadataHash: game.metadataHash, boot: probe.boot };
  }
  outcome(receipt, input, request) {
    if (!receipt) return null;
    invariant(receipt.request === request && receipt.pid === input.pid && receipt.generation === input.generation && receipt.metadataHash === input.metadataHash,
      'FPS_RECEIPT', 'The PS4 counter receipt does not match this game instance.');
    if (receipt.stage === 2 && receipt.resumed === true && receipt.uncertain === false)
      return { completed: true, phase: 'installed', pid: input.pid, generation: input.generation, metadataHash: input.metadataHash,
        message: 'Counter installation recorded. Live measurements are verified separately.' };
    return null;
  }
  handler() {
    const self = this, runtime = this.runtime, native = this.native;
    return {
      validate: input => self.target(input), resources: input => ['native-payload-dispatch', 'game-memory:' + input.generation], timeout: 30000, cancellable: false,
      async run(input, context) {
        for (;;) {
          if (context.signal.aborted) throw clean('FPS_TIMEOUT', 'FPS preparation expired before native dispatch.');
          if (!same(input, runtime.game) || runtime.graph.safe || !enabled(runtime))
            throw clean('FPS_GAME_CHANGED', 'The game or FPS preference changed before dispatch.');
          try { native.bcFpsStart(context.id, input.pid, input.generation, input.titleId, input.metadataHash);break; }
          catch (error) { if (error.code !== 'NATIVE_BUSY') throw error;await self.delay(100); }
        }
        for (;;) {
          const receipt = native.bcFpsPoll(context.id, input.generation); context.evidence?.(receipt);
          const result = self.outcome(receipt, input, context.id);
          if (result) return result;
          if (receipt?.stage === 3 && receipt.resumed === true && !receipt.uncertain) throw clean('FPS_REJECTED', receipt.message);
          invariant(receipt?.stage === 1, 'FPS_UNCERTAIN', receipt?.message || 'The PS4 counter outcome is not confirmed; no retry was issued.');
          invariant(!context.signal.aborted, 'FPS_TIMEOUT', 'The native FPS operation remains recorded and must be reconciled before retrying.');
          await self.delay(100);
        }
      },
      verify: (result, input) => result.completed && result.pid === input.pid && result.generation === input.generation && ['installed', 'game-ended'].includes(result.phase),
      async reconcile(job) {
        const receipt = native.bcFpsPoll(job.id, job.args.generation), result = self.outcome(receipt, job.args, job.id);if (result) return result;
        if (receipt?.stage === 3 && receipt.resumed === true && !receipt.uncertain) return { absent: true };
        if (receipt?.busy) return null;
        const probe = native.systemProbe();if (probe.busy || typeof probe.boot !== 'string' || !Array.isArray(probe.processes)) return null;
        if (probe.boot !== job.args.boot || !probe.processes.some(p => p.pid === job.args.pid && p.generation === job.args.generation))
          return { completed: true, phase: 'game-ended', pid: job.args.pid, generation: job.args.generation, message: 'The original game instance ended; no hook was replayed.' };
        return null;
      }
    };
  }
  async inspect() {
    const runtime = this.runtime, game = runtime.game;
    if (!isPs4(game) || runtime.graph.safe || runtime.recovery?.pending || !enabled(runtime)) { this.observation = null;return; }
    const prior = runtime.jobs.snapshot().filter(job => job.kind === 'fps.ps4.prepare' && job.args.generation === game.generation).at(-1);
    if (prior) {
      const receipt = this.native.bcFpsPoll(prior.id, game.generation); // Recover the RAM cache after a coordinator restart.
      this.outcome(receipt, prior.args, prior.id);
      this.observation = { generation: game.generation, phase: prior.status, message: prior.error?.message || receipt?.message || 'PS4 frame counter is preparing.' };
      return; // Never automatically retry a rejected or interrupted installation.
    }
    if (runtime.jobs.running.size) return;
    const input = this.target();
    await runtime.jobs.enqueue('fps.ps4.prepare', input, runtime.id());
    this.observation = { generation: game.generation, phase: 'queued', message: 'Preparing a frame counter for this PS4 game.' };
  }
  refresh() {
    if (this.pending) return this.pending;
    this.pending = this.inspect().catch(error => {
      this.observation = { generation: this.runtime.game?.generation, phase: 'unavailable', message: error.message };
    }).finally(() => { this.pending = null; });return this.pending;
  }
  start() {
    if (!this.stopped) return;this.stopped = false;
    const run = async () => { if (this.stopped) return;await this.refresh();if (!this.stopped) this.timer = setTimeout(run, 2000); };run();
  }
  stop() { this.stopped = true;clearTimeout(this.timer);this.timer = null; }
}
export function installPs4Fps(runtime, native, options) {
  if (typeof native.bcFpsStart !== 'function' || typeof native.bcFpsPoll !== 'function') return null;
  return runtime.ps4Fps = new Ps4Fps(runtime, native, options);
}
