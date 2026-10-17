// SPDX-License-Identifier: GPL-3.0-or-later
export function payloadRunAction(state, item, componentsByHash) {
  const verified = state.jobs.findLast(job => job.status === 'completed' && job.result?.id === item.id && job.result?.sha256);
  const component = componentsByHash?.get(item.sha256 || verified?.result.sha256 || item.expectedSha256);
  if (component) {
    const service = state.services.find(s => s.id === component);
    const name = { kstuff: 'kstuff', shadowmount: 'ShadowMount', 'payload-manager': 'Payload Manager' }[component];
    const loadable = !state.safe && Boolean(state.capabilities.find(x => x.id === 'payloads')?.available);
    if (service?.status === 'ready') {
      // Already running is not a reason to refuse: the user may run the payload
      // again, and the loader no longer rejects a repeat dispatch.
      return { enabled: loadable, command: loadable ? 'job.enqueue' : null,
        args: { kind: 'payload.dispatch', input: { id: item.id } }, confirm: true,
        detail: loadable ? `${name} is running. Load this payload again in its own process.`
          : `${name} is running; payload loading is not ready.` };
    }
    const enabled = service?.status === 'stopped' && !state.safe;
    return { enabled, command: enabled ? 'service.start' : null, args: { id: component }, confirm: true,
      detail: service?.error?.message ||
        (enabled ? `Start ${name} with its dependencies checked.` : `${name} is ${service?.status || 'unavailable'}. Check its status in Settings.`) };
  }
  const enabled = !state.safe && Boolean(state.capabilities.find(x => x.id === 'payloads')?.available);
  return { enabled, command: enabled ? 'job.enqueue' : null, args: { kind: 'payload.dispatch', input: { id: item.id } },
    confirm: true, detail: enabled ? 'Load this verified ELF now.' : 'Payload loading is not ready.' };
}
