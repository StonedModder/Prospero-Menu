// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant, PmError } from './errors.mjs';
import { localRequest } from './local-http.mjs';
import { StartupRecovery } from './startup-recovery.mjs';
import { expiredStartup, sameExpiry } from './boot-recovery.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const definitions = {
  kstuff: { needle: 'kstuff' },
  shadowmount: { needle: 'shadowmount', port: 10101, route: '/api/v1/version' },
  'payload-manager': { needle: 'pldmgr', port: 8084, route: '/version' }
};
const match = (probe, id) => probe.processes.filter(p => (p.name + ' ' + p.thread).toLowerCase().includes(definitions[id].needle));
const unknown = (reason, reasonCode = 'COMPONENT_UNCONFIRMED') => ({ ready: false, absent: false, reason, reasonCode });
export function serviceInspector(native, { get = localRequest } = {}) {
  return async function inspect(id) {
    invariant(Object.hasOwn(definitions, id), 'SERVICE_UNKNOWN', 'Unknown native component.');
    const first = native.systemProbe();
    if (first.busy) return { ...unknown('Native operation is still running.', 'PROBE_BUSY'), busy: true };
    invariant(typeof first.boot === 'string' && Array.isArray(first.processes), 'SYSTEM_PROBE', 'Native system evidence is incomplete.');
    if (id === 'kstuff') {
      // kstuff readiness comes from kernel snapshots, so we can adopt it even
      // while a native loader operation is still active (e.g. the autoloader
      // is finishing its own copy). Only treat the system as busy if kstuff
      // is not yet installed and something else is occupying the loader.
      if (first.kstuff.state === 'installed' && first.kstuff.injectionReady)
        return { ready: true, id, boot: first.boot, firmware: first.firmware, hooks: 'installed', injectionReady: true };
      if (first.busy) return { ...unknown('Native operation is still running.', 'PROBE_BUSY'), busy: true };
      if (first.kstuff.state === 'unmodified' && !match(first, id).length)
        return { absent: true, ready: false, id, boot: first.boot };
      return unknown('kstuff hooks are changing, incomplete, or unsuitable for injection. No second copy was started.', 'KSTUFF_HOOKS_UNCONFIRMED');
    }
    const processes = match(first, id), port = first.ports[id];
    if (!processes.length && port === 0) return { absent: true, ready: false, id, boot: first.boot };
    if (processes.length !== 1 || port !== 1) return unknown('The component process and listener do not agree.', 'PROCESS_LISTENER_DISAGREE');
    const process = processes[0];
    if (!Number.isInteger(process.pid) || process.pid <= 0 || !process.generation) return unknown('The component process identity is incomplete.', 'PROCESS_IDENTITY_INCOMPLETE');
    let api;
    try {
      const response = await get(definitions[id].port, definitions[id].route, id === 'shadowmount' ? { method: 'POST', body: {} } : {});
      if (id === 'shadowmount') {
        api = JSON.parse(response.text);
        // API v1 is required; individual features are advertised in
        // capabilities and checked where they are used (listing needs
        // list_games, mounting needs mount_game). A build that lists but
        // cannot mount must not make the whole Games page unavailable.
        if (api.status !== 0 || api.api_version !== 1 || typeof api.shadowmount_version !== 'string' || !/^[0-9][A-Za-z0-9.+_-]{0,63}$/.test(api.shadowmount_version) ||
          !Array.isArray(api.capabilities) || !api.capabilities.every(key => typeof key === 'string' && key.length <= 64) || !api.capabilities.includes('list_games'))
          return unknown('ShadowMount must expose API v1 with list_games.', 'SHADOWMOUNT_API_MISMATCH');
      } else {
        const version = response.text.trim();
        if (!/^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$/.test(version) || version.length > 64)
          return unknown('Payload Manager did not return a recognizable version response.', 'MANAGER_VERSION_MISMATCH');
        // Readiness is process identity + a responding service, not ownership of
        // user preferences. Adoption neither rewrites config nor starts a copy.
        // This is not a claim that unprobed APIs in other versions are compatible.
        api = { version };
      }
    } catch (error) { return unknown(error.message || 'The component API is not ready.', 'COMPONENT_API_UNAVAILABLE'); }
    const second = native.systemProbe();
    if (second.busy) return { ...unknown('Native operation deferred the second readiness observation.', 'PROBE_BUSY'), busy: true };
    if (second.boot !== first.boot || second.ports[id] !== 1 || match(second, id).length !== 1 ||
      !match(second, id).some(p => p.pid === process.pid && p.generation === process.generation)) return unknown('The component restarted during readiness verification.', 'COMPONENT_CHANGED');
    if (second.kstuff.state !== 'installed' || !second.kstuff.injectionReady) return unknown('kstuff readiness was lost during component verification.', 'KSTUFF_READINESS_LOST');
    return { ready: true, id, boot: first.boot, pid: process.pid, generation: process.generation,
      version: id === 'shadowmount' ? api.shadowmount_version : api.version,
      capabilities: id === 'shadowmount' ? api.capabilities : [] };
  };
}
const sameInstance = (a, b) => a?.ready === true && b?.ready === true && a.id === b.id && a.boot === b.boot &&
  (a.id === 'kstuff' ? b.hooks === 'installed' && b.injectionReady : a.pid === b.pid && a.generation === b.generation && a.version === b.version);

export function installNativeServices(runtime, native, assets, { delay = sleep, get = localRequest, timeout = 90000 } = {}) {
  const inspect = serviceInspector(native, { get });
  // kstuff, ShadowMount and Payload Manager are never bundled, launched or
  // reconfigured by ProsperoMenu: users autoload their own copies and the menu
  // only detects and adopts them.
  runtime.componentHashes = new Set();
  runtime.componentsByHash = new Map();
  const notRunning = id => Object.assign(new PmError('COMPONENT_NOT_RUNNING',
    `${id === 'payload-manager' ? 'Payload Manager' : id === 'shadowmount' ? 'ShadowMount' : 'kstuff'} is not running. Load it from your own autoloader; the menu adopts it automatically.`,
    { action: 'ProsperoMenu does not bundle or start this payload.' }), { noSideEffects: true });
  runtime.jobs.register('component.start', {
    resources: () => ['native-payload-dispatch'], timeout: timeout + 30000, cancellable: false,
    async validate(input) {
      invariant(Object.hasOwn(definitions, input?.component), 'SERVICE_UNKNOWN', 'Choose a supported component.');
      invariant(!runtime.graph.safe || runtime.recovery?.permits(input.component), 'SAFE_START', 'Automatic native startup is disabled while recovery is pending.');
      const probe = native.systemProbe();
      invariant(!probe.busy && typeof probe.boot === 'string', 'STARTUP_BOOT', 'Current boot identity is unavailable.');
      if (input.boot) invariant(input.boot === probe.boot, 'STARTUP_BOOT_CHANGED', 'This component request belongs to an earlier boot.');
      const evidence = await inspect(input.component);
      if (evidence.absent) throw notRunning(input.component);
      return { component: input.component, boot: probe.boot };
    },
    // Adopt-only. ProsperoMenu never launches kstuff, ShadowMount or Payload
    // Manager and never edits their configuration: it waits briefly for the
    // user's own copy to settle and records it, or reports that it is absent.
    async run(input, context) {
      let evidence;
      try {
        invariant(!runtime.graph.safe || runtime.recovery?.permits(input.component), 'SAFE_START', 'Recovery mode prevents starting a native component.');
        const probeDeadline = Date.now() + timeout;
        while (Date.now() < probeDeadline && !context.signal.aborted) {
          evidence = await inspect(input.component);
          context.evidence?.(evidence);
          if (evidence.ready) return { completed: true, evidence, adopted: true };
          if (evidence.absent) break;
          await context.progress({ completed: 0, total: 1, phase: 'Waiting for ' + input.component + ' to settle' });
          await delay(500);
        }
        throw evidence?.absent ? notRunning(input.component) : new PmError('COMPONENT_CONFLICT', evidence?.reason || 'Existing component state is uncertain.');
      } catch (error) { error.noSideEffects = true; throw error; }
    },
    verify: async result => sameInstance(result.evidence, await inspect(result.evidence?.id)),
    async reconcile(job) {
      const receipt = native.loaderPoll(job.id);
      if (receipt) invariant(receipt.request === job.id && receipt.sha256 === job.args.sha256, 'DISPATCH_RECEIPT', 'The previous startup receipt has a different identity.');
      const expired = expiredStartup(native, job, receipt);if (expired) return expired;
      const evidence = await inspect(job.args.component);
      if (evidence.ready) return { completed: true, evidence, reconciled: true };
      // A spawn receipt plus an absent process is not permission to repeat a
      // kernel operation. Only an unexecuted request can be retried in this boot.
      if (evidence.absent && (!receipt || receipt.stage === 4 && receipt.noSideEffects)) return { absent: true };
      return null;
    },
    verifyExpired: (outcome, job) => sameExpiry(outcome, expiredStartup(native, job, native.loaderPoll(job.id)))
  });
  for (const id of Object.keys(definitions)) {
    runtime.graph.register({ id, dependencies: id === 'kstuff' ? ['coordinator'] : ['kstuff'],
      optional: id !== 'kstuff', timeout: timeout + 60000, inspect: () => inspect(id),
      verify: async evidence => sameInstance(evidence, await inspect(id)),
      async start({ signal }) {
        // A short native operation (an overlay or payload spawn) makes the
        // probe report "busy". That is contention, not evidence of a conflict:
        // wait for it to finish instead of failing ordered startup.
        let existing = await inspect(id);
        for (const until = Date.now() + 30000; existing.busy && !signal.aborted && Date.now() < until;) {
          await delay(250); existing = await inspect(id);
        }
        if (existing.ready) return existing;
        invariant(existing.absent, 'COMPONENT_CONFLICT', existing.reason || 'Component state is unknown.');
        throw notRunning(id);
      }
    });
  }
  runtime.capability('kstuff', true, 'Kernel hook markers are inspected on each readiness check.');
  runtime.recovery = new StartupRecovery(runtime, native);
  let timer = null, monitoring = false, checking = false;
  const misses = new Map();
  async function checkResident() {
    if (checking || runtime.jobs.running.size || runtime.recovery?.pending) return;
    checking = true;
    try {
      // Adopt what the user's autoloader started after the menu, and re-adopt
      // a service that is verifiable again after losing readiness. Nothing is
      // launched here: reconcile only inspects and records.
      for (const id of Object.keys(definitions)) {
        if (runtime.graph.ready(id) || runtime.graph.pending.has(id)) continue;
        let evidence;
        try { evidence = await inspect(id); } catch { continue; }
        const status = runtime.graph.states.get(id)?.status;
        if (evidence.busy || !(evidence.ready || (evidence.absent && status !== 'stopped'))) continue;
        try { await runtime.graph.reconcile(id); } catch { /* Still unverifiable; try again next check. */ }
      }
      for (const id of Object.keys(definitions)) {
        if (!runtime.graph.ready(id)) { misses.delete(id); continue; }
        let evidence;
        try { evidence = await inspect(id); } catch (error) { evidence = unknown(error.message); }
        if (evidence.busy) continue; // Contention supplies no new evidence of service loss.
        if (sameInstance(runtime.graph.states.get(id).evidence, evidence)) { misses.delete(id); continue; }
        const count = (misses.get(id) || 0) + 1; misses.set(id, count);
        if (count < 2) continue;
        runtime.graph.invalidate(id, new PmError('SERVICE_READINESS_LOST', `${id} no longer matches its verified startup instance.`,
          { action: 'Check readiness before using features that depend on it. No automatic replacement was launched.' }));
        await runtime.store.transaction(state => {
          state.history.push({ id: runtime.id(), session: runtime.session, at: runtime.now(), type: 'service', undo: false,
            message: `${id} readiness changed`, detail: evidence.reason || 'Its process, kernel hooks or service identity changed.' });
          state.history = state.history.slice(-256);
        });
      }
    } finally { checking = false; }
  }
  return { inspect, checkResident,
    startMonitoring() {
      if (monitoring) return; monitoring = true;
      const tick = async () => {
        try { await checkResident(); } catch (error) { console.error('Service observation:', error.message); }
        if (monitoring) timer = setTimeout(tick, 10000);
      };
      timer = setTimeout(tick, 10000);
    },
    stopMonitoring() { monitoring = false; clearTimeout(timer); },
    startConfigured: () => runtime.recovery.startConfigured()
  };
}
