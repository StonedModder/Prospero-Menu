// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
import { payloadRunAction } from './deck-payload-action.mjs';

// Continue the user's explicit Load selection after copying. This does not hold
// a queue slot while waiting for a child, and never resumes itself after reboot.
export function continuePayloadLoad(runtime, imported, { delay = ms => new Promise(resolve => setTimeout(resolve, ms)) } = {}) {
  const view = { job: imported.id, phase: 'preparing', name: imported.args?.name || 'Payload', file: null, error: null };
  const wait = async id => {
    for (;;) {
      invariant(!runtime.jobs.stopped, 'PAYLOAD_STOPPED', 'ProsperoMenu stopped before loading the payload.');
      const job = await runtime.jobs.get(id);
      invariant(job, 'PAYLOAD_OPERATION', 'The payload operation is no longer available.');
      if (job.status === 'completed') return job;
      invariant(!['failed','cancelled','interrupted','uncertain','held'].includes(job.status), job.error?.code || 'PAYLOAD_STOPPED',
        job.error?.message || 'Payload preparation stopped. No new load was submitted.');
      await delay(250);
    }
  };
  view.done = (async () => {
    try {
      const prepared = await wait(imported.id);view.file = prepared.result.id;
      const state = runtime.snapshot(), item = state.inbox.find(file => file.id === view.file);
      invariant(item, 'PAYLOAD_MISSING', 'The prepared payload is no longer available.');
      const action = payloadRunAction(state, {...item,sha256:prepared.result.sha256}, runtime.componentsByHash);
      invariant(action.enabled, 'PAYLOAD_UNAVAILABLE', action.detail);
      invariant(!runtime.jobs.stopped, 'PAYLOAD_STOPPED', 'ProsperoMenu stopped before loading the payload.');
      view.phase = 'loading';
      const result = await runtime.execute(action.command, action.command === 'job.enqueue'
        ? {...action.args,key:'storage_load_'+imported.id} : action.args);
      if (action.command === 'job.enqueue') {
        view.job = result.id;const loaded = await wait(result.id);
        view.message = loaded.result?.message || 'Payload sent to the loader.';
      } else view.message = 'Service started and ready.';
      view.phase = 'ready';
    } catch (error) { view.phase = 'error';view.error = error.message; }
  })();
  return view;
}
