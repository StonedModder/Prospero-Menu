// SPDX-License-Identifier: GPL-3.0-or-later
import { ConsoleDeck } from './deck.mjs';
import { invariant, PmError } from './errors.mjs';
import { expiredStartup, sameExpiry } from './boot-recovery.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
// The menu no longer starts kstuff itself (users autoload their own copy), and
// autoloaders commonly start ProsperoMenu before kstuff. Injection requires
// kstuff, so the deck waits for it instead of failing once at boot.
export const KSTUFF_WAIT_MS = 120000;
export function installConsoleDeck(runtime, native, { delay = sleep, timeout = 30000, kstuffWait = KSTUFF_WAIT_MS, now = Date.now } = {}) {
  runtime.consoleDeck = new ConsoleDeck(runtime); runtime.deckCredential = runtime.id();
  const prior = runtime.jobs.snapshot().filter(j => j.kind === 'deck.start').at(-1);
  let request = prior?.id || null, monitor = null, misses = 0;
  function enable(ready) {
    for (const feature of ['deck', 'ip-overlay']) runtime.capability(feature, ready,
      ready ? 'Native ShellUI renderer heartbeat verified.' : 'Native renderer is not ready. Use the phone interface.');
    // The separate game-input relay owns game-overlay readiness. ShellUI's
    // heartbeat alone cannot establish game-process input isolation.
  }
  enable(false);
  function inspect(requestId = request) {
    if (!requestId) return { absent: true, ready: false };
    const status = native.deckPoll(requestId), job = runtime.jobs.snapshot().find(j => j.id === requestId);
    if (job && job.session !== runtime.session && expiredStartup(native, job, status)) return { absent: true, ready: false, expired: true };
    if (job && job.session !== runtime.session && !(status?.stage === 4 && status.noSideEffects))
      return { absent: false, ready: false, reason: 'The earlier coordinator may still own ShellUI hooks in this boot. Restart the console before starting a new deck.' };
    return status ? { ...status, absent: status.stage === 4 && status.noSideEffects } : { ready: false, absent: true };
  }
  function target(input) {
    const state = native.systemProbe();
    invariant(!state.busy && state.kstuff?.state === 'installed' && state.kstuff.injectionReady,
      'DECK_DEPENDENCY', 'Verified kstuff must be ready before the native deck starts.');
    const shells = state.processes.filter(p => p.name === 'SceShellUI');
    invariant(shells.length === 1 && shells[0].generation, 'SHELLUI_IDENTITY', 'ShellUI process identity could not be verified.');
    // A process name does not establish ownership of our hook sites.
    // The native preparer checks their actual instructions before publication.
    if (input?.generation) invariant(input.pid === shells[0].pid && input.generation === shells[0].generation,
      'SHELLUI_CHANGED', 'ShellUI restarted after the native deck was requested.');
    return { pid: shells[0].pid, generation: shells[0].generation, ...(state.boot ? { boot: state.boot } : {}) };
  }
  runtime.jobs.register('deck.start', {
    validate: target, resources: () => ['native-payload-dispatch', 'shellui-hooks'], timeout: timeout + 30000, cancellable: false,
    async run(input, context) {
      try { target(input); } catch (error) { error.noSideEffects = true; throw error; }
      request = context.id;
      try { native.deckStart(request, input.pid, input.generation, runtime.deckCredential); }
      catch (error) {
        // The native binding creates a plain JS Error, not a PmError. Preserve
        // its known check/errno instead of replacing it with OPERATION_FAILED.
        if (['DECK_NATIVE', 'DECK_ALREADY_ATTEMPTED', 'NATIVE_BUSY'].includes(error?.code)) {
          console.log('ProsperoMenu deck startup: ' + error.code + ' ' + error.message);
          throw new PmError(error.code, error.message, { action: 'Review Activity before retrying. Native deck startup was not confirmed.' });
        }
        console.log('ProsperoMenu deck startup exception: ' + String(error?.stack || error?.message || error).slice(0, 220));
        throw error;
      }
      const deadline = Date.now() + timeout;
      while (Date.now() < deadline && !context.signal.aborted) {
        const status = inspect();
        context.evidence?.(status);
        invariant(status.request === request && status.pid === input.pid && status.generation === input.generation,
          'DECK_RECEIPT', 'Native deck receipt does not match this ShellUI process.');
        if (status.ready) return { completed: true, evidence: status };
        if (status.stage === 4 && status.noSideEffects) throw Object.assign(new PmError('DECK_REJECTED', status.message), { noSideEffects: true });
        invariant(status.stage !== 5, 'DECK_UNCERTAIN', status.message || 'Deck startup needs inspection before retrying.');
        await context.progress({ completed: 0, total: 1, phase: status.message || 'Waiting for native renderer' });
        await delay(250);
      }
      throw new PmError('DECK_NOT_READY', 'The native deck did not produce a fresh render heartbeat. Use the phone interface; no reinjection was issued.');
    },
    verify(result, input) { const status = inspect(); return status.ready && status.pid === input.pid && status.generation === input.generation && result.evidence.request === status.request; },
    reconcile(job) {
      const receipt = native.deckPoll(job.id), expired = expiredStartup(native, job, receipt);if (expired) return expired;
      const status = inspect(job.id); if (status.absent) return { absent: true };
      return status.ready && status.pid === job.args.pid && status.generation === job.args.generation ? { completed: true, evidence: status } : null;
    },
    verifyExpired: (outcome, job) => sameExpiry(outcome, expiredStartup(native, job, native.deckPoll(job.id)))
  });
  // kstuff is an injection precondition, checked by target() and the native
  // worker. Its monitoring state must not invalidate a healthy resident menu.
  function kstuffReady() {
    try { const state = native.systemProbe();return !state.busy && state.kstuff?.state === 'installed' && state.kstuff.injectionReady === true; }
    catch { return false; }
  }
  runtime.graph.register({ id: 'native-deck', dependencies: ['coordinator'], optional: true, timeout: timeout + 55000 + kstuffWait,
    inspect, verify: evidence => evidence?.request === inspect().request && inspect().ready,
    async start({ signal }) {
      // stdlib console.error goes to stderr, which never reaches runtime.log, so
      // a deck that fails to start left no trace at all. Every branch below is
      // logged to stdout on purpose.
      console.log('ProsperoMenu deck: node start requested');
      const current = inspect(); if (current.ready) { enable(true); return current; }
      console.log('ProsperoMenu deck: current state ' + JSON.stringify({ absent: !!current.absent, ready: !!current.ready, reason: current.reason || null, stage: current.stage ?? null }));
      invariant(current.absent, 'DECK_UNCERTAIN', 'The previous native deck attempt needs inspection.');
      if (!kstuffReady()) {
        console.log('ProsperoMenu deck: waiting for kstuff from the autoloader');
        const until = now() + kstuffWait;
        while (!signal.aborted && now() < until && !kstuffReady()) await delay(500);
        console.log('ProsperoMenu deck: kstuff ' + (kstuffReady() ? 'ready' : 'still not ready; trying anyway'));
      }
      // While the autoloader is still injecting other payloads, one process
      // census can miss ShellUI's identity (seen on 13.60 the moment kstuff
      // became ready). That check runs before anything is injected, so retry it.
      for (const until = now() + 20000; !signal.aborted; await delay(500)) {
        try { target();break; }
        catch (error) {
          if (!['SHELLUI_IDENTITY', 'DECK_DEPENDENCY'].includes(error.code) || now() >= until) break;
          console.log('ProsperoMenu deck: ' + error.code + ' before injection; checking again');
        }
      }
      // A bootstrap rejected before publishing hooks is recorded as a clean
      // rejection: one retry is safe and absorbs the kernel-hook readiness race
      // (the arena mprotect flakily fails on some boots). Published hooks or an
      // uncertain attempt are never retried.
      for (let attempt = 0; attempt < 3; attempt += 1) {
        const job = await runtime.jobs.enqueue('deck.start', {}, 'deck_' + (runtime.recovery?.attempt || runtime.session) + (attempt ? '_r' + attempt : ''));
        console.log('ProsperoMenu deck: job enqueued ' + job.id.slice(0, 12));
        let ended = null;
        while (!signal.aborted) {
          const state = runtime.jobs.snapshot().find(j => j.id === job.id);
          if (state.status === 'completed') { enable(true); return state.result.evidence; }
          if (['failed', 'interrupted', 'cancelled', 'held', 'uncertain'].includes(state.status)) { ended = state; break; }
          await delay(250);
        }
        if (!ended) throw new PmError('DECK_WAIT_EXPIRED', 'Deck startup remains recorded in Activity.');
        console.log('ProsperoMenu deck: job ended ' + ended.status + ' ' + (ended.error?.code || '') + ' ' + (ended.error?.message || ''));
        const cleanRejection = ended.error?.code === 'DECK_REJECTED' || ended.error?.noSideEffects;
        if (cleanRejection && attempt < 2) {
          console.log('ProsperoMenu deck: clean early rejection; retrying (' + (attempt + 1) + '/2)');
          await delay(750);
          continue;
        }
        for (const feature of ['deck', 'ip-overlay']) runtime.capability(feature, false, ended.error?.message || 'Native renderer startup failed. Review Activity.');
        throw new PmError(ended.error?.code || 'DECK_STOPPED', ended.error?.message || 'Native deck startup needs inspection.');
      }
    }
  });
  async function check() {
    if (!request || runtime.jobs.running.size) return;
    const status = inspect();
    if (status.busy) return;
    const wasReady = runtime.capabilities.get('deck').available;
    // One missed heartbeat is not a lost renderer. Dropping the capability
    // cleared the in-game input target and closed an open menu; require two
    // consecutive misses (about 4 s) before disabling a working deck.
    if (status.ready) misses = 0;
    else if (wasReady && ++misses < 2) return;
    if (wasReady !== Boolean(status.ready)) enable(Boolean(status.ready));
    // A temporarily absent scene can recover naturally; never publish the
    // hooks again. The same injection's new render heartbeat re-enables it.
  }
  return runtime.nativeDeck = {
    inspect, check,
    startMonitoring() { if (!monitor) monitor = setInterval(() => { try { check().catch(() => enable(false)); } catch { enable(false); } }, 2000); },
    stopMonitoring() { clearInterval(monitor); monitor = null; }
  };
}
