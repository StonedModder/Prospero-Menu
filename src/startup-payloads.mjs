// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { clone, invariant, publicError } from './errors.mjs';
import { payloadDispatchHandler } from './payload-loader.mjs';
import { expiredStartup, sameExpiry } from './boot-recovery.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const terminal = status => ['completed', 'failed', 'cancelled', 'interrupted', 'held', 'uncertain'].includes(status);
export function validateStartupPayloads(value = []) {
  invariant(Array.isArray(value) && value.length <= 32 && new Set(value.map(x => x?.sha256)).size === value.length,
    'STARTUP_PAYLOAD_CONFIG', 'Choose at most 32 different startup payloads.');
  for (const item of value) invariant(item && /^[a-f0-9]{32}$/.test(item.id) && /^[a-f0-9]{64}$/.test(item.sha256) &&
    typeof item.name === 'string' && item.name.length <= 180 && typeof item.enabled === 'boolean' &&
    [0, 2000, 5000, 10000].includes(item.delayMs), 'STARTUP_PAYLOAD_CONFIG', 'Invalid startup payload selection.');
  return value;
}
export function installStartupPayloads(runtime, native, { delay = sleep } = {}) {
  let pending = null, stopped = false;
  let status = { phase: 'waiting', message: 'Startup payloads run after ProsperoMenu services are ready.' };
  const dispatch = payloadDispatchHandler(runtime, native, { delay });
  const boot = () => {
    const probe = native.systemProbe();
    invariant(!probe.busy && typeof probe.boot === 'string' && probe.boot.length > 0 && probe.boot.length <= 80,
      'STARTUP_BUSY', 'Wait for the current console operation to finish.');
    return probe.boot;
  };
  runtime.jobs.register('payload.startup', {
    ...dispatch,
    async validate(input) {
      invariant(input.boot === boot(), 'STARTUP_BOOT_CHANGED', 'This startup request belongs to an earlier console boot.');
      return { ...await dispatch.validate(input), boot: input.boot };
    },
    async run(input, context) {
      try {
        invariant(input.boot === boot() && !runtime.graph.safe, 'STARTUP_HELD', 'Startup payloads are held until normal startup completes.');
        const selected = runtime.store.snapshot().startupPayloads?.find(x => x.sha256 === input.sha256);
        invariant(selected?.enabled && selected.id === input.id, 'STARTUP_DISABLED', 'This payload was removed from startup before it ran.');
        // The same verified bytes may have been loaded manually or through FTP.
        // Check durable native receipts inside the serialized dispatch resource.
        for (const job of runtime.jobs.snapshot()) {
          if (job.id === context.id || !['payload.dispatch', 'payload.startup'].includes(job.kind) || job.args.sha256 !== input.sha256) continue;
          const receipt = native.loaderPoll(job.id);
          if (!receipt || receipt.boot !== input.boot) continue;
          invariant(receipt.request === job.id && receipt.sha256 === input.sha256, 'DISPATCH_RECEIPT', 'An existing payload receipt did not match.');
          if (receipt.stage === 3) return { completed: true, phase: 'already-dispatched', sha256: input.sha256,
            initialized: false, request: receipt.request, pid: receipt.pid, message: 'Already loaded during this console boot; skipped duplicate.' };
          invariant(receipt.stage === 4 && receipt.noSideEffects, 'DISPATCH_UNCERTAIN', 'An earlier load needs checking. Startup will not load a second copy.');
        }
      } catch (error) { error.noSideEffects = true; throw error; }
      return dispatch.run(input, context);
    },
    verify: (result, input) => dispatch.verify(result.phase === 'already-dispatched' ? { ...result, phase: 'dispatched' } : result, input),
    reconcile: job => expiredStartup(native, job, native.loaderPoll(job.id)) || dispatch.reconcile(job),
    verifyExpired: (outcome, job) => sameExpiry(outcome, expiredStartup(native, job, native.loaderPoll(job.id)))
  });
  async function configure({ id, enabled, delayMs, move, remove, revision }) {
    invariant(Number.isSafeInteger(revision), 'REVISION_REQUIRED', 'Refresh the payload list before changing startup.');
    invariant(remove === undefined || remove === true, 'STARTUP_PAYLOAD_CONFIG', 'Invalid startup removal.');
    if (remove) {
      await runtime.store.transaction(state => { state.startupPayloads = (state.startupPayloads || []).filter(item => item.id !== id); }, revision);
      runtime.sequence++;return snapshot();
    }
    invariant(enabled === undefined || typeof enabled === 'boolean', 'STARTUP_PAYLOAD_CONFIG', 'Choose On or Off.');
    invariant(delayMs === undefined || [0, 2000, 5000, 10000].includes(delayMs), 'STARTUP_PAYLOAD_CONFIG', 'Choose a supported startup delay.');
    invariant(move === undefined || [-1, 1].includes(move), 'STARTUP_PAYLOAD_CONFIG', 'Choose an earlier or later startup position.');
    const existing = runtime.store.snapshot().startupPayloads?.find(x => x.id === id);
    // A missing file must never prevent turning off or reordering its entry.
    const changingOnly = existing && enabled !== true;
    const file = changingOnly ? existing : await runtime.inbox.status(id), info = changingOnly ? existing : await dispatch.validate({ id });
    await runtime.store.transaction(state => {
      const list = state.startupPayloads ||= [];
      let index = list.findIndex(x => x.sha256 === info.sha256);
      if (index < 0) { invariant(list.length < 32, 'STARTUP_PAYLOAD_LIMIT', 'Remove an unused startup entry first.'); list.push({ id, sha256: info.sha256, name: file.name, enabled: false, delayMs: 2000 }); index = list.length - 1; }
      const item = list[index]; item.id = id; item.name = file.name;
      if (enabled !== undefined) item.enabled = enabled;
      if (delayMs !== undefined) item.delayMs = delayMs;
      if (move && index + move >= 0 && index + move < list.length) [list[index], list[index + move]] = [list[index + move], list[index]];
    }, revision);
    runtime.sequence++; return snapshot();
  }
  function snapshot() { return { items: clone(runtime.store.snapshot().startupPayloads || []), ...status,
    available: runtime.inboxItems.filter(item => item.status === 'verified' && /\.elf$/i.test(item.name) &&
      !runtime.componentHashes?.has(item.expectedSha256)).map(({ id, name, expectedSha256 }) => ({ id, name, sha256: expectedSha256 })) }; }
  async function run() {
    if (pending) return pending;
    pending = (async () => {
      if (stopped || runtime.graph.safe || runtime.recovery?.pending) return;
      const required = runtime.recovery?.snapshot()?.last;
      if (required && required.status !== 'ready') return;
      const plan = clone(runtime.store.snapshot().startupPayloads || []).filter(x => x.enabled);
      if (!plan.length) { status = { phase: 'idle', message: 'No payloads selected for startup.' }; return; }
      const currentBoot = boot();
      invariant((await runtime.nativeServices.inspect('kstuff')).ready, 'KSTUFF_NOT_READY', 'Startup payloads are waiting for kstuff.');
      status = { phase: 'running', message: 'Loading selected startup payloads.' }; runtime.sequence++;
      for (const item of plan) {
        if (stopped) return;
        const key = 'startup_' + createHash('sha256').update(currentBoot + '/' + item.sha256).digest('hex');
        // Reusing this durable key returns its original intent, even after a
        // coordinator restart. Failed/uncertain dispatch is never replayed.
        let job = await runtime.jobs.enqueue('payload.startup', { id: item.id, sha256: item.sha256, boot: currentBoot }, key);
        while (!terminal(job.status) && !stopped) { await delay(200); job = await runtime.jobs.get(job.id); }
        if (stopped) return;
        invariant(job.status === 'completed', 'STARTUP_PAYLOAD_STOPPED', job.error?.message || 'A startup payload needs attention. Remaining payloads were held.');
        if (item.delayMs) await delay(item.delayMs);
      }
      status = { phase: 'completed', message: 'Selected startup payloads were dispatched. Check their own notifications for readiness.' };
    })().catch(error => { status = { phase: 'attention', message: publicError(error).message }; })
      .finally(() => { pending = null; runtime.sequence++; });
    return pending;
  }
  runtime.startupPayloads = { configure, snapshot, run, stop() { stopped = true; } };
  return runtime.startupPayloads;
}
