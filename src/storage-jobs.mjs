// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant, PmError } from './errors.mjs';
import { sameService } from './library.mjs';
const integer = x => Number.isSafeInteger(x) && x >= 0;
const path = x => typeof x === 'string' && x.length <= 1024 && x.startsWith('/') &&
  !/[\x00-\x1f\\]/.test(x) && !x.slice(1).split('/').some(part => part === '.' || part === '..' || part === '');
// Root slash is allowed, empty interior/trailing segments are not.
const absolute = path;
const PHASES = new Set(['preparing', 'measuring', 'transferring', 'finalizing', 'completed', 'failed', 'cancelled']);
const terminal = s => ['completed', 'failed', 'cancelled'].includes(s.state);
export function installStorageJobs(runtime, { pollMs = 1000, deadline = 48 * 60 * 60 * 1000 } = {}) {
  const library = runtime.library;
  invariant(library, 'STORAGE_LIBRARY', 'Storage requires the ShadowMount library.');
  const call = (route, body, accepted = [200]) => library.call(route, body, 256 * 1024, accepted);
  async function destinations() {
    const service = await library.ready(), data = await call('storage', {});
    invariant(Array.isArray(data.destinations) && data.destinations.length <= 64, 'STORAGE_RESPONSE', 'Invalid storage destination list.');
    const items = data.destinations.map(item => {
      invariant(absolute(item.path) && absolute(item.mount_point) && typeof item.source === 'string' && item.source.length <= 1024 &&
        typeof item.filesystem === 'string' && item.filesystem.length <= 64 && typeof item.read_only === 'boolean' &&
        integer(item.available_bytes) && integer(item.total_bytes) && item.available_bytes <= item.total_bytes,
        'STORAGE_RESPONSE', 'Incomplete storage capacity information.');
      return { path: item.path, mount: item.mount_point, source: item.source, filesystem: item.filesystem,
        availableBytes: item.available_bytes, totalBytes: item.total_bytes, readOnly: item.read_only };
    });
    invariant(sameService(service, await library.ready()), 'SERVICE_CHANGED', 'ShadowMount restarted while reading storage.');
    return { destinations: items.filter(item => !item.readOnly), service };
  }
  async function validate(input) {
    invariant(input && ['copy', 'move', 'unpack'].includes(input.operation) && absolute(input.destination), 'STORAGE_INPUT', 'Choose an operation and a listed destination.');
    const current = await library.scan(), item = current.games.find(game => game.titleId === input.titleId);
    invariant(item?.canMount && item.sourceAvailable && absolute(item.path), 'STORAGE_SOURCE', 'Choose an available managed game source.');
    invariant(input.operation !== 'unpack' || item.sourceType === 'image', 'STORAGE_FORMAT', 'Only game images can be extracted to folders.');
    invariant(input.path === item.path && input.contentId === item.contentId, 'LIBRARY_CHANGED', 'The game source changed. Refresh before continuing.');
    if (input.service) invariant(sameService(input.service, current.service), 'SERVICE_CHANGED', 'ShadowMount restarted after this request.');
    const available = await destinations(), destination = available.destinations.find(d => d.path === input.destination);
    invariant(sameService(current.service, available.service) && destination, 'STORAGE_DESTINATION', 'The chosen destination is no longer available.');
    if (input.volume) invariant(input.volume.source === destination.source && input.volume.mount === destination.mount &&
      input.volume.filesystem === destination.filesystem, 'STORAGE_CHANGED', 'The destination volume changed after this request.');
    const output = destination.path + '/' + (input.operation === 'unpack' ? item.titleId : item.path.split('/').pop());
    invariant(output !== item.path && !output.startsWith(item.path + '/'), 'STORAGE_RECURSIVE', 'The destination must be outside the source.');
    return { operation: input.operation, titleId: item.titleId, path: item.path, sourceType: item.sourceType,
      contentId: item.contentId, destination: destination.path, output, volume: destination, service: current.service };
  }
  function receipt(data, input, id = null) {
    invariant(data && integer(data.job_id) && data.job_id > 0 && (id === null || data.job_id === id) &&
      data.operation === input.operation && data.title_id === input.titleId && data.source === input.path &&
      data.destination === input.output && data.source_type === input.sourceType && data.delete_source === false &&
      PHASES.has(data.state) && typeof data.active === 'boolean' && data.active === !terminal(data) &&
      typeof data.cancellable === 'boolean' && integer(data.result_status) && integer(data.processed_bytes) &&
      integer(data.total_bytes) && data.processed_bytes <= data.total_bytes,
      'STORAGE_RECEIPT', 'The storage operation no longer matches this request. No retry was sent.');
    return data;
  }
  async function status(input, id) {
    invariant(sameService(input.service, await library.ready()), 'SERVICE_CHANGED', 'The storage service restarted; its old operation identity cannot be reused.');
    const current = receipt(await call('games/storage/status', { job_id: id }), input, id);
    invariant(sameService(input.service, await library.ready()), 'SERVICE_CHANGED', 'The storage service changed during verification.');
    return current;
  }
  function outcome(current) {
    if (current.state === 'completed' && current.result_status === 0)
      return { completed: true, jobId: current.job_id, destination: current.destination, message: 'Storage operation completed and confirmed by ShadowMount.' };
    if (current.state === 'cancelled') return { cancelled: true, jobId: current.job_id, message: 'ShadowMount confirmed cancellation.' };
    if (current.state === 'failed') throw new PmError('STORAGE_FAILED', 'ShadowMount reported a storage failure.',
      { action: 'Keep both source and destination until their contents have been checked.' });
    return null;
  }
  runtime.jobs.register('library.storage', {
    validate, resources: () => ['shadowmount-library'], timeout: deadline, cancellable: true,
    async run(input, context) {
      try { await validate(input); } catch (error) { error.noSideEffects = true; throw error; }
      if (context.signal.aborted) return { cancelled: true };
      const body = { title_id: input.titleId, destination_dir: input.destination };
      if (input.operation === 'unpack') body.delete_source = false;
      const accepted = receipt(await call('games/' + input.operation, body, [202]), input);
      // This is written before polling and is never exposed as a user command.
      // A lost acceptance response remains uncertain; we never guess the ID.
      await context.checkpoint({ jobId: accepted.job_id });
      let cancelSent = false;
      for (;;) {
        const current = await status(input, accepted.job_id), result = outcome(current);
        if (result) return result;
        await context.progress({ completed: current.processed_bytes, total: current.total_bytes,
          phase: current.state + (context.signal.aborted && !current.cancellable ? ' · Finishing safely; cancellation is unavailable in this phase.' : '') });
        if (context.signal.aborted && current.cancellable && !cancelSent) {
          cancelSent = true;
          // Finalization can race cancellation. An HTTP conflict is not proof
          // of cancellation; continue observing the existing operation.
          await call('games/storage/cancel', { job_id: accepted.job_id }, [200, 409]).catch(() => {});
        }
        await new Promise(resolve => setTimeout(resolve, pollMs));
      }
    },
    async verify(result, input) {
      const current = await status(input, result.jobId);
      return current.state === 'completed' && current.result_status === 0 && current.destination === result.destination;
    },
    async reconcile(job) {
      invariant(integer(job.checkpoint?.jobId) && job.checkpoint.jobId > 0, 'STORAGE_RECEIPT_MISSING', 'No confirmed operation identity survived. The operation will not be restarted automatically.');
      const current = await status(job.args, job.checkpoint.jobId), result = outcome(current);
      return result?.cancelled ? { absent: true } : result;
    }
  });
  runtime.storageJobs = { destinations };
  runtime.capability('shadowmount', true, 'Game library, verified mounts, background copy/move and image extraction with progress.');
  return runtime.storageJobs;
}
