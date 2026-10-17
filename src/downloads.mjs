// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant, PmError } from './errors.mjs';
import { downloadUrl, redirectedDownloadUrl } from './download-url.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const MAX_PAYLOAD = 128 * 1048576;
export function installDownloads(runtime, native, { delay = sleep, root = '/data/ProsperoMenu/downloads' } = {}) {
  // Reusable transport for payloads and the cheat-source updater. No HTTP
  // response or redirect can choose a filesystem destination or execute code.
  async function fetchToFile(url, limit, context, { timeoutSeconds = 900 } = {}) {
    const seen = new Set();let current = downloadUrl(url), started = false;
    try {
      for (let hop = 0; hop <= 8; hop++) {
        invariant(!seen.has(current), 'DOWNLOAD_REDIRECT_LOOP', 'The download link redirects in a loop. Use the final direct link.');seen.add(current);
        invariant(!context.signal?.aborted, 'DOWNLOAD_CANCELLED', 'Download cancelled.');
        native.downloadStart(context.id, current, limit, timeoutSeconds);started = true;
        let receipt;
        for (;;) {
          if (context.signal?.aborted) native.downloadCancel(context.id);
          receipt = native.downloadPoll(context.id);
          invariant(receipt?.request === context.id, 'DOWNLOAD_RECEIPT', 'The download worker could not be checked.');
          await context.progress({ completed: receipt.bytes, total: receipt.total || limit,
            phase: receipt.total ? 'Downloading file' : 'Downloading file · size not supplied by server', indeterminate: !receipt.total });
          if (receipt.phase !== 1) break;
          await delay(200);
        }
        invariant(!context.signal?.aborted && receipt.phase !== 5, 'DOWNLOAD_CANCELLED', 'Download cancelled.');
        if (receipt.phase === 3) {
          const next = redirectedDownloadUrl(receipt.location, current);
          native.downloadRelease(context.id);started = false;current = next;continue;
        }
        invariant(receipt.phase === 2, 'DOWNLOAD_FAILED', receipt.message || 'Download failed.',
          { action: receipt.status && receipt.status !== 200 ? `Server returned HTTP ${receipt.status}. Use a direct file link.` : 'Check the link and connection, then try again.' });
        return { source: root + '/' + context.id + '.download', bytes: receipt.bytes,
          release() { if (started) { native.downloadRelease(context.id);started = false; } } };
      }
      throw new PmError('DOWNLOAD_REDIRECT_LIMIT', 'This link has too many redirects. Use the final direct link.');
    } catch (error) {
      // A cancellation deadline never frees a worker that is still writing.
      if (started) {
        native.downloadCancel(context.id);
        for (;;) { const receipt = native.downloadPoll(context.id);if (!receipt || receipt.phase !== 1) break;await delay(200); }
        if (native.downloadPoll(context.id)) native.downloadRelease(context.id);
      }
      throw error;
    }
  }
  runtime.downloads = { fetchToFile };
  runtime.jobs.register('payload.download', {
    validate(input) {
      invariant(input && typeof input.name === 'string' && /^[^/\\\x00-\x1f]{1,176}\.elf$/i.test(input.name),
        'DOWNLOAD_NAME', 'Give this payload a filename ending in .elf.');
      return { url: downloadUrl(input.url), name: input.name };
    },
    resources: () => ['native-download'], timeout: 15 * 60 * 1000, cancellable: true,
    async run(input, context) {
      let file;
      try {
        file = await fetchToFile(input.url, MAX_PAYLOAD, context);
        const imported = await runtime.inbox.importFile(file.source, input.name, context);
        return { completed: true, id: imported.id, sha256: imported.sha256,
          message: 'Payload downloaded and verified. Choose Run or add it to Startup payloads.' };
      } catch (error) {
        if (['DOWNLOAD_CANCELLED', 'UPLOAD_CANCELLED'].includes(error.code)) return { cancelled: true, message: 'Download cancelled. No payload was run.' };
        error.noSideEffects = true;throw error;
      } finally {
        file?.release();runtime.inboxItems = await runtime.inbox.list();runtime.sequence++;
      }
    },
    async verify(result) { return (await runtime.inbox.status(result.id)).status === 'verified'; },
    // Downloading never launches a payload. An interrupted download is safe to
    // abandon; completed inbox imports remain visible for deliberate use.
    async reconcile() { return { absent: true }; }
  });
  return runtime.downloads;
}
