// SPDX-License-Identifier: GPL-3.0-or-later
import { inspectGameMetadata, GAME_TITLE } from './game-metadata.mjs';
import { invariant, clone, publicError } from './errors.mjs';
const same = (a, b) => a && b && ['pid', 'generation', 'appId', 'titleId', 'version', 'metadataHash'].every(key => a[key] === b[key]);
export function selectGameProcess(processes) {
  invariant(Array.isArray(processes) && processes.length <= 4096, 'GAME_PROCESSES', 'Application process snapshot is invalid.');
  const candidates = processes.filter(p => GAME_TITLE.test(p.titleId) && Number.isSafeInteger(p.pid) && p.pid > 1 &&
    Number.isSafeInteger(p.appId) && p.appId > 0 && typeof p.generation === 'string' && p.generation.length > 0 && p.generation.length < 128);
  if (!candidates.length) return null;
  const titles = new Set(candidates.map(p => p.titleId + ':' + p.appId));
  invariant(titles.size === 1, 'GAME_AMBIGUOUS', 'More than one application is active; game modifications are held.');
  const main = candidates.filter(p => p.name === 'eboot.bin');
  invariant(main.length === 1 || candidates.length === 1, 'GAME_AMBIGUOUS', 'The main application process could not be identified uniquely.');
  const chosen = main.length === 1 ? main[0] : candidates[0];
  return { pid: chosen.pid, generation: chosen.generation, appId: chosen.appId, titleId: chosen.titleId, process: chosen.name };
}
export class GameLifecycle {
  constructor(runtime, native, { now = Date.now, interval = 2000, absenceGrace = 1500 } = {}) {
    this.runtime = runtime; this.native = native; this.now = now; this.interval = interval; this.listeners = new Set();
    this.current = null; this.candidate = null; this.pending = null; this.timer = null; this.stopped = true; this.lease = null; this.missingSince = 0; this.absenceGrace = absenceGrace;
    runtime.gameStatus = { phase: 'unverified', message: 'Running game has not been inspected.', at: 0 };
  }
  subscribe(listener) { this.listeners.add(listener); return () => this.listeners.delete(listener); }
  async emit(type, game, previous = null) {
    const failed = error => {
      this.runtime.lifecycleErrors = [...(this.runtime.lifecycleErrors || []), { type, at: this.now(), error: publicError(error) }].slice(-16);
    };
    for (const listener of this.listeners) {
      try { Promise.resolve(listener({ type, game: clone(game), previous: clone(previous), signal: this.lease?.signal || null })).catch(failed); }
      catch (error) { failed(error); }
    }
  }
  state(phase, message, details = {}) {
    const value = { phase, message, ...details, at: this.now() };
    this.runtime.gameStatus = value; this.runtime.sequence++;
    this.runtime.diagnostics?.game(phase, this.runtime.game, details.error);return value;
  }
  async invalidate(message, error = null) {
    const changed = this.runtime.gameStatus.phase !== 'uncertain' || this.runtime.gameStatus.message !== message;
    this.lease?.abort(); this.runtime.game = null; this.candidate = null; this.state('uncertain', message, { error });
    // Unknown is not proof of process exit; lifecycle consumers must withhold
    // writes and never redirect old handles into a new/reused PID.
    if (changed) await this.emit('unavailable', null, this.current);
  }
  async inspect() {
    if (this.runtime.recovery?.pending) return;
    this.deferred = false;
    const probe = this.native.systemProbe();
    if (probe.busy) {
      // A confirmed live, short input-exchange worker only touches its private
      // callback block. Retry the census after it returns instead of declaring
      // the game unavailable on every ordinary controller exchange.
      if (this.runtime.gameInputRelay?.observationBusy()) { this.deferred = true;return; }
      await this.invalidate('A native operation is active; game actions are temporarily held.'); return;
    }
    invariant(probe.appInfoAvailable === true, 'GAME_API_UNAVAILABLE', 'Native application identity API is unavailable.');
    const process = selectGameProcess(probe.processes);
    // A process census without the running game is not an exit: on 13.60 the
    // table (or the game's app info) occasionally drops it for a moment.
    // Publishing an exit then a rediscovery reset in-game input and closed the
    // menu several times a minute. Require the absence to last 1.5 s, rechecked
    // every 25 ms; a real exit is still published promptly.
    if (!process && this.current) {
      this.missingSince ||= this.now();
      if (this.now() - this.missingSince < this.absenceGrace) { this.deferred = true;return; }
    }
    this.missingSince = 0;
    if (!process) {
      const previous = this.current; this.lease?.abort(); this.current = this.candidate = null; this.runtime.game = null;
      this.state('idle', 'No running game detected.'); if (previous) await this.emit('exit', null, previous); return;
    }
    const mounted = this.native.gameMetadata(process.pid, process.generation, process.titleId);
    const registered = mounted?.scope === 'registered-running-title' && mounted.format === 'sfo' && /^CUSA\d{5}$/.test(process.titleId) &&
      mounted.generation === process.generation && mounted.titleId === process.titleId;
    invariant(mounted?.scope === 'running-sandbox' || registered, 'GAME_METADATA_SOURCE', 'Game actions require the running application’s metadata.');
    const game = { ...process, ...inspectGameMetadata(mounted.format, mounted.bytes, process.titleId) };
    if (!same(this.candidate, game)) {
      const wasAvailable = Boolean(this.runtime.game); this.lease?.abort();
      this.candidate = game; this.runtime.game = null; this.state('discovering', 'Confirming the running game instance.', { titleId: game.titleId });
      if (wasAvailable) await this.emit('unavailable', null, this.current); return;
    }
    const previous = this.current, changed = !same(previous, game), recovering = this.runtime.gameStatus.phase !== 'ready';
    this.current = game; this.runtime.game = clone(game); this.state('ready', 'Running title, version and process instance verified.');
    if (changed) {
      this.lease?.abort(); if (previous) await this.emit('exit', null, previous);
      this.lease = new AbortController(); await this.emit('start', game, previous);
    } else if (recovering) { this.lease = new AbortController(); await this.emit('available', game); }
  }
  refresh() {
    if (this.pending) return this.pending;
    this.pending = this.inspect().catch(async error => { await this.invalidate(error.message, publicError(error)); })
      .finally(() => { this.pending = null; }); return this.pending;
  }
  start() {
    if (!this.stopped) return; this.stopped = false;
    const tick = async () => { if (this.stopped) return; await this.refresh(); if (!this.stopped) this.timer = setTimeout(tick,
      this.deferred ? 25 : this.runtime.gameStatus.phase === 'discovering' ? 150 : this.runtime.gameStatus.phase === 'idle' ? 1000 : this.interval); };
    tick();
  }
  stop() { this.stopped = true; this.lease?.abort(); clearTimeout(this.timer); this.timer = null; }
}
export function installGameLifecycle(runtime, native, options) {
  return runtime.gameLifecycle = new GameLifecycle(runtime, native, options);
}
