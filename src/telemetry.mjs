// SPDX-License-Identifier: GPL-3.0-or-later
// Samples are observations, not a process lease or proof of rendered frames.
// AGC/DCE counter interpretation credits OnionHEN/LightningMods and PHU/ArkSama.
const decimal = value => typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value) && BigInt(value) <= 0xffffffffffffffffn ? BigInt(value) : null;
const rounded = n => Math.round(n * 10) / 10;
const empty = reason => ({ fps: null, fpsSource: null, cpuPercent: null, rssBytes: null, reason });
const targetKey = (game, flags) => game ? JSON.stringify([game.pid, game.generation, game.titleId, flags]) : '';

export class CounterWindow {
  constructor() { this.reset(); }
  reset() { this.previous = null;this.rates = { ring: [], global: [], scanout: [], bc: [] };this.pairs = []; }
  consume(sample) {
    const previous = this.previous;this.previous = sample;
    const delta = previous ? sample.time - previous.time : 0;
    const interval = Number.isFinite(delta) && delta >= 100 && delta <= 3000;
    const result = empty('Collecting a fresh counter interval.');
    if (Number.isSafeInteger(sample.rssBytes) && sample.rssBytes >= 0 && sample.rssBytes <= 128 * 1024 ** 3) result.rssBytes = sample.rssBytes;
    const cpus = sample.cpus, cpu = decimal(sample.cpuUsec), beforeCpu = decimal(previous?.cpuUsec);
    if (interval && Number.isInteger(cpus) && cpus >= 1 && cpus <= 64 && cpus === previous.cpus && cpu !== null && beforeCpu !== null && cpu >= beforeCpu) {
      const percent = Number(cpu - beforeCpu) / (delta * 1000 * cpus) * 100;
      if (Number.isFinite(percent) && percent <= 100) result.cpuPercent = rounded(percent);
    }
    const rates = {};
    for (const key of ['ring', 'global', 'scanout', 'bc']) {
      const count = decimal(sample[key]), prior = decimal(previous?.[key]);
      const raw = interval && count !== null && prior !== null && count > prior ? Number(count - prior) * 1000 / delta : null;
      if (raw === null || raw < 1 || raw > 10000) { this.rates[key] = [];continue; }
      this.rates[key].push(raw);if (this.rates[key].length > 5) this.rates[key].shift();
      rates[key] = this.rates[key].reduce((a, b) => a + b, 0) / this.rates[key].length;
    }
    const source = rates.bc ? 'bc' : rates.ring ? 'ring' : rates.global ? 'global' : null;
    // A render counter can count several submissions per displayed frame.
    // Never silently relabel display scanout as game FPS when that is detected.
    if (source && rates.scanout >= 25) {
      const ratio = rates[source] / rates.scanout;
      if (ratio >= 0.5 && ratio <= 200) this.pairs.push({ source, ratio });
      else this.pairs = [];
      if (this.pairs.length > 100) this.pairs.shift();
    } else this.pairs = [];
    const multipass = source && this.pairs.length === 100 && this.pairs.every(p => p.source === source) &&
      this.pairs.reduce((sum, p) => sum + p.ratio, 0) / 100 > 2.5;
    if (source && rates[source] <= 240 && !multipass) {
      result.fps = rounded(rates[source]);result.fpsSource = source === 'bc' ? 'gnm-flip-submissions' : source === 'ring' ? 'agc-ring' : 'agc-submissions';
      result.reason = 'Observed game render counter; hardware validation pending.';
    } else if (rates.scanout >= 1 && rates.scanout <= 240) {
      result.fps = rounded(rates.scanout);result.fpsSource = 'display-scanout';
      result.reason = 'Display refresh measurement; game frame rate is unavailable.';
    } else if (source) result.reason = 'Render submissions do not establish a valid game frame rate.';
    return result;
  }
}

export class Telemetry {
  constructor(runtime, native, { now = Date.now, interval = 500 } = {}) {
    this.runtime = runtime;this.native = native;this.now = now;this.interval = interval;
    this.window = new CounterWindow();this.pending = null;this.key = '';this.last = null;
    this.next = 0;this.timer = null;this.stopped = true;
  }
  target() {
    const settings = this.runtime.overlaySettings?.() || this.runtime.store.snapshot().settings.overlay;
    const flags = (settings.ram || settings.cpu ? 1 : 0) | (settings.fps ? 2 : 0);
    const game = this.runtime.graph.safe || this.runtime.recovery?.pending ? null : this.runtime.game;
    return { game, flags, key: flags ? targetKey(game, flags) : '' };
  }
  reset() {
    this.key = '';this.last = null;this.window.reset();this.next = 0;
    if (this.pending) this.pending.discard = true;
  }
  tick() {
    const target = this.target(), now = this.now();
    if (target.key !== this.key) { this.reset();this.key = target.key; }
    if (this.pending) {
      const pending = this.pending, sample = this.native.telemetryPoll(pending.serial);
      if (!sample) return; // Never release/relaunch a still-running native worker.
      this.pending = null;
      if (!pending.discard && pending.key === target.key && sample.serial === pending.serial && sample.valid === true &&
          sample.pid === target.game?.pid && sample.generation === target.game?.generation &&
          Number.isSafeInteger(sample.time) && sample.time > 0 && now >= pending.at && now - pending.at <= 3000) {
        this.last = { ...this.window.consume(sample), at: now, pid: sample.pid, generation: sample.generation };
        this.runtime.sequence++;
      } else { this.last = null;this.window.reset(); }
      this.next = now + this.interval;
    }
    if (!target.key || now < this.next || this.runtime.jobs.running.size) return;
    const serial = this.native.telemetryStart(target.game.pid, target.game.generation, target.game.titleId, target.flags);
    this.next = now + this.interval;
    if (Number.isInteger(serial) && serial > 0) this.pending = { serial, key: target.key, at: now, discard: false };
  }
  snapshot() {
    const { game, flags, key } = this.target(), now = this.now();
    let out = empty(!flags ? 'Monitoring is off.' : !game ? 'Waiting for a verified running game.' : 'Waiting for fresh game measurements.');
    if (key && this.last && key === this.key && now >= this.last.at && now - this.last.at <= 3000) out = { ...this.last, reason: this.last.reason };
    if (this.pending && (now < this.pending.at || now - this.pending.at > 3000)) out = empty('The native sample is still pending; no additional sampler was started.');
    if (!(flags & 1)) { out.cpuPercent = null;out.rssBytes = null; }
    if (!(flags & 2)) { out.fps = null;out.fpsSource = null; }
    if (flags & 2 && game && !/^PPS[AB]/.test(game.titleId) && out.fps === null) {
      const observed = this.runtime.ps4Fps?.observation;
      out.reason = observed?.generation === game.generation ? observed.message : this.runtime.ps4Fps ? 'Waiting for this PS4 game’s frame counter.' : 'PS4 frame-counter integration is not ready in this build.';
    }
    return out;
  }
  start() {
    if (!this.stopped) return;this.stopped = false;
    const run = () => {
      if (this.stopped) return;
      try { this.tick(); } catch { this.last = null;this.window.reset(); }
      this.timer = setTimeout(run, this.pending ? 250 : this.target().key ? this.interval : 2000);
    };run();
  }
  stop() { this.stopped = true;clearTimeout(this.timer);this.timer = null;this.reset(); }
}

export function telemetryText(state) {
  const options = state.settings.overlay, sample = state.telemetry, parts = [];
  const number = (value, unit) => value == null ? 'unavailable' : value + unit;
  if (options.fps) parts.push((sample?.fpsSource === 'display-scanout' ? 'Display ' : 'FPS ') + number(sample?.fps, ''));
  if (options.cpu) parts.push('CPU ' + number(sample?.cpuPercent, '%'));
  if (options.ram) parts.push('RAM ' + number(sample?.rssBytes == null ? null : Math.round(sample.rssBytes / 1024 ** 2), ' MiB'));
  return parts.join('   |   ');
}
export function installTelemetry(runtime, native, options) {
  if (typeof native.telemetryStart !== 'function' || typeof native.telemetryPoll !== 'function') return null;
  runtime.capability('fps', true, runtime.ps4Fps ? 'Native PS4 GNM and PS5 AGC/DCE counters with a shared readout. Hardware untested.' : 'Native PS5 AGC/DCE sampler and shared readout; PS4 counter integration remains pending. Hardware untested.');
  runtime.capability('resources', true, 'Native process CPU and resident RAM observations. CPU is a share of total logical CPU capacity. Hardware untested.');
  return runtime.telemetry = new Telemetry(runtime, native, options);
}
