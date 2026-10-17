// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
// Embedded components remain outside the JS heap until streamed to the inbox.
// A stored matching filename is never treated as proof of valid content.
export class ComponentAssets {
  constructor(inbox, manifest, read) { this.inbox = inbox; this.manifest = manifest; this.read = read; this.pending = new Map(); }
  get(id) {
    const component = this.manifest.find(item => item.id === id);
    invariant(component, 'COMPONENT_UNKNOWN', 'No pinned component exists for this service.');
    return component;
  }
  ensure(id) {
    if (this.pending.has(id)) return this.pending.get(id);
    const operation = this.prepare(id); this.pending.set(id, operation);
    operation.finally(() => { if (this.pending.get(id) === operation) this.pending.delete(id); }).catch(() => {});
    return operation;
  }
  async prepare(id) {
    const component = this.get(id);
    const prior = (await this.inbox.list()).filter(item => item.expectedSha256 === component.sha256 && item.bytes === component.bytes);
    const entry = prior.find(item => item.status === 'verified') || prior.find(item => item.status === 'uploading') ||
      await this.inbox.begin({ name: component.file, bytes: component.bytes, sha256: component.sha256 });
    if (entry.status !== 'verified') {
      invariant(entry.offset >= 0 && entry.offset <= component.bytes, 'COMPONENT_PARTIAL', 'Saved component upload has invalid length.');
      for (let offset = entry.offset; offset < component.bytes;) {
        const size = Math.min(1024 * 1024, component.bytes - offset);
        const data = Buffer.from(await this.read(component.offset + offset, size));
        invariant(data.length === size, 'COMPONENT_SHORT_READ', 'The embedded component could not be read completely.');
        await this.inbox.append(entry.id, offset, data); offset += size;
      }
    }
    const verified = await this.inbox.verify(entry.id);
    invariant(verified.sha256 === component.sha256 && verified.format === 'elf', 'COMPONENT_CHANGED', 'Bundled component verification failed.');
    return { id: verified.id, sha256: verified.sha256 };
  }
}
