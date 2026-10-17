// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
const uint = x => Number.isInteger(x) && x >= 0 && x <= 0xffffffff;
// Freshness windows, in console milliseconds. The ShellUI renderer reaches the
// coordinator over loopback HTTP, and on 13.60 one round trip takes 100-150 ms.
// An exchange collects every event the game produced since the previous one,
// so the oldest event in a healthy batch is about one exchange interval old.
// Older events are dropped individually: rejecting the whole batch reset the
// epoch, which closed the menu and dropped held chords every few hundred ms.
export const EVENT_WINDOW_MS = 400;
// A batch is replaced once it has been sent to the renderer. Waiting for the
// renderer's acknowledgement as well doubled the exchange interval.
export const BATCH_UNDELIVERED_MS = 500;
// With a game in front, ShellUI renders the deck less often (heartbeat age is
// ~160 ms and spikes past 300 ms). A missed spike reset input as "renderer
// stale". The lease (400 ms) still bounds how long the game input is held.
export const RENDER_WINDOW_MS = 500;
export class GameInputRelay {
  constructor(runtime, native) {
    this.runtime = runtime;this.native = native;this.epoch = 1;this.serial = 0;this.key = '';this.pending = null;
    this.batch = null;this.feedback = null;this.owners = new Map();this.deadline = 0;this.capture = null;this.timer = null;this.stopped = true;
    this.log = [];this.routeResets = null;this.dropped = 0; // ProsperoMenu diagnostics
  }
  now() { return this.native.gameInputClock(); }
  target() {
    const r = this.runtime, game = r.game;
    if (!game || r.graph.safe || r.recovery?.pending || !r.capabilities.get('deck').available || r.gameInput?.observation?.generation !== game.generation || r.gameInput.observation.phase !== 'prepared') return null;
    return { ...game, key: game.generation + ':' + game.metadataHash };
  }
  rendered(now) {
    const f = this.feedback;
    return f?.ready && now >= f.at && now - f.at <= RENDER_WINDOW_MS && now >= f.renderedAt && now - f.renderedAt <= RENDER_WINDOW_MS;
  }
  observationBusy() {
    const p = this.pending, now = this.now();
    return Boolean(p && p.key === this.target()?.key && now >= p.at && now - p.at <= 200 &&
      !this.runtime.jobs.running.size && this.native.gameInputExchangePoll(p.serial)?.busy);
  }
  note(reason) { this.log.push({ at: this.now(), epoch: this.epoch, reason });if (this.log.length > 40) this.log.shift(); }
  reset(reason = 'unspecified') {
    this.note('reset:' + reason);
    this.epoch = (this.epoch + 1) >>> 0 || 1;this.serial = 0;this.batch = null;this.deadline = 0;this.capture = null;this.owners.clear();
  }
  receive(input) {
    try { return this.accept(input); } catch (error) { this.note('rejected:' + (error.code || 'error')); throw error; }
  }
  accept(input) {
    invariant(input && uint(input.epoch) && uint(input.serial) && typeof input.open === 'boolean' && typeof input.ready === 'boolean' &&
      Number.isInteger(input.owner) && input.owner >= -1 && input.owner <= 0x7fffffff && uint(input.connection) && input.connection <= 255 &&
      Number.isSafeInteger(input.renderedAt) && input.renderedAt >= 0, 'INPUT_FEEDBACK', 'Invalid native renderer input feedback.', { status: 400 });
    const now = this.now();
    invariant(input.renderedAt <= now, 'INPUT_FEEDBACK_TIME', 'Renderer timestamp is ahead of the console clock.', { status: 400 });
    if (input.open && input.epoch === this.epoch) {
      const owner = this.owners.get(input.owner);
      invariant(owner && owner.connection === input.connection && now >= owner.at && now - owner.at <= 500,
        'INPUT_OWNER', 'The requesting controller is no longer verified.', { status: 409 });
    }
    if (Array.isArray(input.routeResets)) this.routeResets = input.routeResets.slice(0, 4);
    this.feedback = { ...input, at: now };
    if (input.epoch === this.epoch && this.batch && input.serial === this.batch.serial) this.batch = null;
  }
  tick() {
    const now = this.now(), target = this.target(), key = target?.key || '';
    if (key !== this.key) { this.reset('target-changed');this.key = key; }
    const ready = now > 0 && target && this.rendered(now);
    if (this.pending) {
      const pending = this.pending, sample = this.native.gameInputExchangePoll(pending.serial);
      if (sample?.busy) {
        if ((!ready || now < pending.at || now - pending.at > 200) && pending.epoch === this.epoch) this.reset(!ready ? 'busy-not-ready' : 'busy-timeout');
        return; // Never replace a worker that has not actually completed.
      }
      this.pending = null;
      if (ready && pending.epoch === this.epoch && pending.key === key && now >= pending.at && now - pending.at <= 200 &&
        sample?.serial === pending.serial && sample.pid === target.pid && sample.generation === target.generation &&
        sample.metadataHash === target.metadataHash && sample.epoch === this.epoch && sample.result === 0 &&
        sample.time > 0 && sample.time <= now + 1 && sample.time >= pending.at && sample.deadline > now && sample.deadline - now <= 400 && Array.isArray(sample.events) && sample.events.length <= 128 &&
        sample.events.every(e => uint(e.sequence) && uint(e.buttons) && Number.isInteger(e.handle) && e.handle >= 0 && e.handle <= 0x7fffffff &&
          uint(e.connection) && e.connection <= 255 && e.time > 0 && e.time <= now + 1)) {
        if (this.serial === 0xffffffff) { this.reset('serial-wrap');return; }
        // A stale press is never replayed, but it does not invalidate the
        // fresh events behind it or the lease this exchange just renewed.
        const events = sample.events.filter(e => now - e.time <= EVENT_WINDOW_MS);
        this.dropped += sample.events.length - events.length;
        this.batch = { serial: ++this.serial, events, at: now, delivered: false };
        const continued = this.capture?.owner === pending.owner && this.capture?.connection === pending.connection && this.deadline > now;
        this.capture = pending.mode === 2 ? { owner: pending.owner, connection: pending.connection, at: continued ? this.capture.at : sample.time } : null;
        this.deadline = sample.deadline;
        for (const [handle, owner] of this.owners) if (now - owner.at > 2000) this.owners.delete(handle);
        for (const e of events) this.owners.set(e.handle, { connection: e.connection, at: e.time });
        if (this.owners.size > 16) { this.reset('owners-overflow');return; }
      } else if (pending.epoch === this.epoch) this.reset(!ready ? 'sample-not-ready' : !sample ? 'sample-missing' : sample.result !== 0 ? 'sample-result-' + sample.result : 'sample-rejected');
    }
    if (!ready) { if (this.deadline || this.batch) this.reset(!target ? 'no-target' : 'renderer-stale');return; }
    if (this.batch && !this.batch.delivered) { if (now - this.batch.at > BATCH_UNDELIVERED_MS) this.reset('batch-undelivered');else return; }
    if (this.runtime.jobs.running.size) return;
    const feedback = this.feedback, owner = this.owners.get(feedback.owner);
    const capture = feedback.epoch === this.epoch && feedback.open && owner && owner.connection === feedback.connection && now - owner.at <= 500;
    const mode = capture ? 2 : 1;
    try {
      const serial = this.native.gameInputExchange(target.pid, target.generation, target.metadataHash, this.epoch, mode, capture ? feedback.owner : -1, capture ? feedback.connection : 0);
      invariant(uint(serial) && serial > 0, 'INPUT_EXCHANGE', 'Invalid input exchange identity.');
      this.pending = { serial, epoch: this.epoch, key, at: now, mode, owner: feedback.owner, connection: feedback.connection };
    } catch (error) { if (error.code !== 'NATIVE_BUSY') { this.reset('exchange-' + (error.code || 'error'));throw error; } }
  }
  // Called once the frame carrying this batch has been encoded into a renderer
  // reply. The renderer de-duplicates by serial, so the next exchange may start.
  delivered(input) {
    if (this.batch && input && input.epoch === this.epoch && input.serial === this.batch.serial) this.batch.delivered = true;
  }
  frame() {
    const now = this.now(), target = this.target();
    const active = target && target.key === this.key && this.rendered(now) && this.deadline > now ? 1 :
      this.runtime.game || (this.runtime.gameStatus && this.runtime.gameStatus.phase !== 'idle') ? 2 : 0;
    return { epoch: this.epoch, serial: this.batch?.serial || this.serial, active, captured: active === 1 && Boolean(this.capture),
      owner: this.capture?.owner ?? -1, connection: this.capture?.connection ?? 0,
      captureAt: active === 1 ? this.capture?.at || 0 : 0,
      deadline: active === 1 ? Math.min(this.deadline, this.feedback.at + 300) : 0, events: active === 1 ? this.batch?.events || [] : [] };
  }
  // ProsperoMenu diagnostics: why the in-game overlay is or isn't ready.
  debug() {
    const now = this.now(), f = this.feedback, target = this.target();
    return { now, target: Boolean(target), observation: this.runtime.gameInput?.observation?.phase ?? null,
      feedback: f ? { ready: f.ready, open: f.open, owner: f.owner, ageMs: now - f.at, renderedAgeMs: f.renderedAt ? now - f.renderedAt : -1 } : null,
      rendered: Boolean(f) && this.rendered(now), pending: Boolean(this.pending), batch: Boolean(this.batch), delivered: Boolean(this.batch?.delivered), dropped: this.dropped, owners: this.owners.size,
      deadlineInMs: this.deadline ? this.deadline - now : null, epoch: this.epoch, serial: this.serial, active: target ? this.frame().active : 0,
      capture: this.capture, routeResets: this.routeResets, log: this.log.slice(-25) };
  }
  start() {
    if (!this.stopped) return;this.stopped = false;
    this.timer = setInterval(() => {
      try { this.tick(); } catch (error) { this.reset('tick-' + (error?.code || 'error')); }
      const ready = this.frame().active === 1 && [...this.owners.values()].some(owner => this.now() - owner.at <= 300);
      if (this.runtime.capabilities.get('game-overlay').available !== ready)
        this.runtime.capability('game-overlay', ready, ready ? 'Game input and native renderer are responding.' : 'Waiting for current game input and native renderer.');
    }, 25);
  }
  stop() { this.stopped = true;clearInterval(this.timer);this.timer = null;this.reset('stopped');this.runtime.capability('game-overlay', false, 'Game input relay stopped.'); }
}
export function installGameInputRelay(runtime, native) {
  if (!['gameInputClock', 'gameInputExchange', 'gameInputExchangePoll'].every(k => typeof native[k] === 'function')) return null;
  return runtime.gameInputRelay = new GameInputRelay(runtime, native);
}
