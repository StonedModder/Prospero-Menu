// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant, PmError } from './errors.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const cleanError = (code, message) => Object.assign(new PmError(code, message), { noSideEffects: true });
// Queue completion means dispatch was verified. Generic payloads do not expose
// a readiness protocol; native services must implement independent probes.
export function payloadDispatchHandler(runtime, native, { delay = sleep, managed = false } = {}) {
  const validate = async input => {
    invariant(input && typeof input.id === 'string', 'PAYLOAD_INPUT', 'Choose a verified inbox payload.');
    const info = await runtime.inbox.verify(input.id);
    invariant(info.format === 'elf', 'PAYLOAD_KIND', 'This menu only dispatches standalone ELF payloads; other container types cannot be run here.');
    invariant(managed || !runtime.componentHashes?.has(info.sha256), 'MANAGED_COMPONENT',
      'Start this bundled component from Services so its dependencies and readiness are checked.');
    if (input.sha256) invariant(input.sha256 === info.sha256, 'PAYLOAD_CHANGED', 'Payload changed since this operation was queued.');
    return { id: input.id, sha256: info.sha256 };
  };
  function outcome(receipt) {
    if (receipt?.stage !== 3) return null;
    return { completed: true, phase: 'dispatched', initialized: false, request: receipt.request,
      sha256: receipt.sha256, pid: receipt.pid, message: 'Payload dispatched. Its initialization has not been confirmed.' };
  }
  return {
    validate, resources: () => ['native-payload-dispatch'], timeout: 120000, cancellable: false,
    async run(input, context) {
      // Bind the accepted upload to its bytes once again immediately before
      // native dispatch. The native worker hashes its own private buffer too.
      try { await validate(input); }
      catch (error) { error.noSideEffects = true; throw error; }
      // Short controller/native workers share this guard. Busy means nothing
      // was dispatched, so wait instead of reporting an interrupted launch.
      for (;;) {
        if(context.signal.aborted)throw cleanError('DISPATCH_CANCELLED','Payload dispatch ended before the native loader became available.');
        try { native.loaderStart(context.id, input.id, input.sha256);break; }
        catch(error) {
          if(error.code==='LOADER_BUSY'){await delay(100);continue;}
          if(error.code==='LOADER_NATIVE')throw new PmError(error.code,String(error.message).slice(0,384));
          throw error;
        }
      }
      let phase = 0;
      for (;;) {
        const receipt = native.loaderPoll(context.id);
        context.evidence?.(receipt);
        invariant(receipt?.request === context.id && receipt.sha256 === input.sha256,
          'DISPATCH_RECEIPT', 'The payload dispatch receipt could not be verified.');
        if (phase !== receipt.stage) {
          phase = receipt.stage;
          await context.progress({ completed: phase === 3 ? 1 : 0, total: 1,
            phase: phase === 1 ? 'Preparing payload' : phase === 2 ? 'Starting payload' : 'Checking dispatch result' });
        }
        const result = outcome(receipt); if (result) return result;
        if (phase === 4 && receipt.noSideEffects) throw cleanError('PAYLOAD_REJECTED', receipt.message);
        if (phase === 4 || phase === 5) throw new PmError('DISPATCH_UNCERTAIN', receipt.message,
          { action: 'Inspect this operation before retrying. No automatic retry was issued.' });
        invariant(phase === 1 || phase === 2, 'DISPATCH_RECEIPT', 'Unknown native dispatch phase.');
        // An aborted queue deadline does not stop a native worker. Keep the
        // resource reserved until the worker reports a terminal outcome.
        await delay(250);
      }
    },
    verify: async (result, input) => result.phase === 'dispatched' && result.initialized === false &&
      result.sha256 === input.sha256 && Number.isInteger(result.pid) && result.pid > 0 &&
      /^[a-f0-9]{32}$/.test(result.request),
    async reconcile(job) {
      const receipt = native.loaderPoll(job.id);
      if (!receipt) return { absent: true };
      invariant(receipt.request === job.id && receipt.sha256 === job.args.sha256, 'DISPATCH_RECEIPT', 'Dispatch identity differs from this job.');
      if (receipt.stage === 4 && receipt.noSideEffects) return { absent: true };
      return outcome(receipt);
    }
  };
}
export function installPayloadLoader(runtime, native, options) {
  runtime.jobs.register('payload.dispatch', payloadDispatchHandler(runtime, native, options));
  runtime.capability('payloads', true, 'Verified ELF dispatch with durable receipts. Payload initialization is reported separately.');
}
