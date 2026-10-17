// SPDX-License-Identifier: GPL-3.0-or-later
export function startupPayloadRows(state, view, managed = new Set()) {
  const rows = [], row = (label, detail, command = null, args = {}, extra = {}) => rows.push({ label, detail, command, args, enabled: Boolean(command), ...extra });
  const list = state.startupPayloads?.items || [];
  if (!view.id) {
    row('Startup order', state.startupPayloads?.message || 'Selected payloads load after ProsperoMenu services.');
    const files = [...list, ...state.inbox.filter(x => x.status === 'verified' && /\.elf$/i.test(x.name) && !managed.has(x.expectedSha256) && !list.some(p => p.sha256 === x.expectedSha256))];
    for (const item of files) row(item.name, item.enabled ? `On · Order ${list.filter(x => x.enabled).indexOf(item) + 1}` : 'Off · Select to configure', '$startup-payload-open', { id: item.id });
    if (!files.length) row('Add a payload first', 'Load an ELF from storage or download one in Payloads.');
    return rows;
  }
  const item = list.find(x => x.id === view.id), file = state.inbox.find(x => x.id === view.id), current = item || file;
  if (!current) { row('File no longer available', 'Return to Payloads and add the file again.');return rows; }
  const args = { id: view.id, revision: state.revision };
  row(current.name, 'Changes apply on the next console startup.');
  row('Load when ProsperoMenu starts', item?.enabled ? 'On' : 'Off', 'payload.startup.configure', { ...args, enabled: !item?.enabled }, { on: Boolean(item?.enabled) });
  const delays = [0, 2000, 5000, 10000], delay = item?.delayMs ?? 2000;
  row('Wait after loading', delay ? `${delay / 1000} seconds · Select to change` : 'No delay · Select to change', 'payload.startup.configure', { ...args, delayMs: delays[(delays.indexOf(delay) + 1) % delays.length] });
  if (item && list.indexOf(item) > 0) row('Move earlier', 'Load before the previous payload.', 'payload.startup.configure', { ...args, move: -1 });
  if (item && list.indexOf(item) < list.length - 1) row('Move later', 'Load after the next payload.', 'payload.startup.configure', { ...args, move: 1 });
  if (item) row('Remove from startup list', 'Keep the payload file. Stop loading it automatically.', 'payload.startup.configure', { ...args, remove: true });
  row('Back to startup list', '', '$startup-payloads');return rows;
}
