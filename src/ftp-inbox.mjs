// SPDX-License-Identifier: GPL-3.0-or-later
import { publicError } from './errors.mjs';
// Called only after a complete STOR has atomically replaced its destination.
// Ordinary FTP transfers do not execute code. Inbox autorun is a separate opt-in.
export function ftpInboxReceiver(runtime) {
  return async ({ virtual, target }) => {
    if (!/^\/payloads\/[^/]+\.(elf|plugin|prx|sprx)$/i.test(virtual)) return null;
    const name = virtual.slice('/payloads/'.length); let result, failure;
    try {
      result = await runtime.inbox.importFile(target, name);
      runtime.inboxItems = await runtime.inbox.list(); runtime.sequence++;
      if (runtime.store.snapshot().settings.services.inboxAutoRun && result.format === 'elf') {
        await runtime.jobs.enqueue('payload.dispatch', { id: result.id, sha256: result.sha256 }, 'ftp-' + result.id);
      }
    } catch (error) { failure = publicError(error); }
    try {
      await runtime.store.transaction(state => {
        state.history.push({ id: runtime.id(), session: runtime.session, at: runtime.now(), type: 'ftp-inbox',
          message: failure ? `FTP upload saved; ${name} needs attention` : `${name} added to the payload inbox`,
          detail: failure?.message || 'Verified file; dispatch is tracked as a separate operation.', undo: false });
        state.history = state.history.slice(-256);
      });
    } catch { return 'Transfer saved; inbox status could not be recorded. Check the deck.'; }
    runtime.sequence++;
    return failure ? `Transfer saved; inbox: ${failure.message}` : 'Transfer complete; payload verified in the ProsperoMenu inbox';
  };
}
