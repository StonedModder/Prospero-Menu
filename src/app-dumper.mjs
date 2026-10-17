// SPDX-License-Identifier: GPL-3.0-or-later
// App Dumper overlay client. EchoStretch/ps5-app-dumper runs an ELF payload
// whose only interface is HTTP: it lists what is installed, starts a title,
// dumps a mounted title to a drive and reports live progress. This module is
// that client. The payload itself is never bundled; the overlay drives the copy
// the user runs, exactly like its own web page does.
import { invariant } from './errors.mjs';
import { localRequest } from './local-http.mjs';

// The payload tries 8081 first and walks up to nine ports further (it avoids
// the homebrew launcher's 8080), so the probe has to follow it.
export const PORTS = Object.freeze([8081, 8082, 8083, 8084, 8085, 8086, 8087, 8088, 8089, 8090]);
const PROBE_TTL_MS = 5000;

export function progressText(copied, total) {
  if (!Number.isFinite(total) || total <= 0) return '';
  const ratio = Math.max(0, Math.min(1, copied / total));
  const filled = Math.round(ratio * 10);
  return '\u2588'.repeat(filled) + '\u2591'.repeat(10 - filled) + ' ' + Math.round(ratio * 100) + '%';
}

export function rateText(job) {
  const done = Number(job?.copiedBytes) || 0, total = Number(job?.totalBytes) || 0;
  const started = Number(job?.copyStarted) || 0, now = Number(job?.now) || 0;
  const seconds = now - started;
  if (!done || seconds <= 0) return '';
  const rate = done / seconds;
  const left = total > done && rate > 0 ? Math.round((total - done) / rate) : null;
  const size = value => value >= 1024 * 1024 * 1024 ? (value / 1024 ** 3).toFixed(1) + ' GB'
    : value >= 1024 * 1024 ? Math.round(value / 1024 ** 2) + ' MB' : Math.round(value / 1024) + ' KB';
  return `${size(rate)}/s${left != null ? ' · ' + (left >= 3600 ? Math.round(left / 3600) + ' h' : left >= 60 ? Math.round(left / 60) + ' min' : left + ' s') + ' left' : ''}`;
}

export function installAppDumper(runtime, { request = localRequest, ports = PORTS, now = () => Date.now() } = {}) {
  let cached = { port: null, at: 0 };
  async function fetchJson(port, route, options = {}) {
    const response = await request(port, route, { acceptedStatuses: [200, 400, 404, 409, 500], timeout: 6000, ...options });
    let data = null;
    try { data = JSON.parse(response.text); } catch { data = null; }
    return { status: response.status || 200, data, text: response.text };
  }
  // The payload is optional: a console that is not running it is a normal state
  // the page explains, never an exception.
  async function locate(force = false) {
    if (!force && cached.port && now() - cached.at < PROBE_TTL_MS) return cached.port;
    for (const port of ports) {
      try {
        const { data } = await fetchJson(port, '/api/status');
        if (data && data.job && typeof data.job.state === 'string') { cached = { port, at: now() }; return port; }
      } catch { /* try the next port */ }
    }
    cached = { port: null, at: now() };
    return null;
  }
  async function open(route, options) {
    const port = await locate();
    invariant(port, 'DUMPER_NOT_RUNNING', 'The App Dumper payload is not running. Load ps5-app-dumper.elf from Payloads, then refresh.');
    try { return await fetchJson(port, route, options); }
    catch (error) {
      cached = { port: null, at: 0 };
      throw new Error('The App Dumper stopped answering: ' + (error.message || error));
    }
  }
  const status = async () => {
    const port = await locate();
    if (!port) return { running: false, port: null };
    try {
      const { data } = await fetchJson(port, '/api/status');
      if (!data?.job) { cached = { port: null, at: 0 }; return { running: false, port: null }; }
      return { running: true, port, base: `127.0.0.1:${port}`, job: data.job, queue: data.queue, busy: !!data.busy };
    } catch { cached = { port: null, at: 0 }; return { running: false, port: null }; }
  };
  const library = async () => (await open('/api/library')).data;
  const devices = async () => (await open('/api/devices')).data;
  async function start({ app, target, overwrite = 0 }) {
    invariant(typeof app === 'string' && app.length > 0 && app.length < 512 && !/[\x00-\x1f]/.test(app), 'DUMPER_INPUT', 'Choose a title to dump.');
    invariant(typeof target === 'string' && target.length > 0 && target.length < 256 && !/[\x00-\x1f]/.test(target), 'DUMPER_INPUT', 'Choose a destination drive.');
    const query = new URLSearchParams({ app, target, overwrite: String(overwrite ? 1 : 0) });
    const { data } = await open('/api/dump?' + query.toString(), { method: 'POST' });
    invariant(data?.started, 'DUMPER_START', data?.error || 'The App Dumper did not start the dump.');
    return data;
  }
  const abort = async () => (await open('/api/abort', { method: 'POST' })).data;
  runtime.dumper = { status, library, devices, start, abort, locate, reset: () => { cached = { port: null, at: 0 }; } };
  return runtime.dumper;
}
