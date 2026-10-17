// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { invariant } from './errors.mjs';
import { inspectPayload, MAX_PAYLOAD } from './payload-format.mjs';

export class PayloadInbox {
  constructor(root, { id, policy = null, reservationGroup = null }) {
    this.root = root; this.id = id; this.locks = new Map();
    this.reservations = reservationGroup || { inboxes: new Set(), tail: Promise.resolve() };
    this.policy = policy || { minimum: 64, maximum: MAX_PAYLOAD,
      names: /^[^/\\\x00-\x1f]{1,180}\.(elf|plugin|prx|sprx)$/i,
      nameMessage: 'Choose an ELF or supported plugin.', sizeMessage: 'Payload size must be between 64 bytes and 128 MiB.',
      inspect: async (_file, prefix, status) => inspectPayload(prefix, status.name, { totalBytes: status.bytes }) };
  }
  async init() {
    await fs.mkdir(this.root, { recursive: true });
    invariant(!(await fs.lstat(this.root)).isSymbolicLink(), 'INBOX_PATH', 'The inbox cannot be a symbolic link.');
    this.reservations.inboxes.add(this);
    return this;
  }
  file(id, suffix) {
    invariant(/^[a-f0-9]{32}$/.test(id), 'UPLOAD_ID', 'Invalid upload identity.', { status: 400 });
    return path.join(this.root, id + suffix);
  }
  exclusive(id, action) {
    const prior = this.locks.get(id) || Promise.resolve();
    const operation = prior.catch(() => {}).then(action); this.locks.set(id, operation);
    operation.finally(() => { if (this.locks.get(id) === operation) this.locks.delete(id); }).catch(() => {});
    return operation;
  }
  async syncDirectory() {
    if (process.platform === 'win32') return;
    const directory = await fs.open(this.root, 'r');
    try { await directory.sync(); } finally { await directory.close(); }
  }
  async begin({ name, bytes, sha256 }) {
    return this.allocate({ name, bytes, sha256 }, 0);
  }
  // Only trusted staging code can credit a file already on this same volume.
  // Public upload arguments cannot reduce the storage reservation.
  async allocate({ name, bytes, sha256 }, existingBytes = 0) {
    const operation = this.reservations.tail.catch(() => {}).then(async () => {
    invariant(typeof name === 'string' && this.policy.names.test(name), 'PAYLOAD_NAME', this.policy.nameMessage);
    invariant(Number.isSafeInteger(bytes) && bytes >= this.policy.minimum && bytes <= this.policy.maximum, 'PAYLOAD_SIZE', this.policy.sizeMessage);
    invariant(sha256 == null || /^[a-f0-9]{64}$/.test(sha256), 'PAYLOAD_HASH', 'Invalid expected SHA-256.');
    const existing = (await fs.readdir(this.root)).filter(name => name.endsWith('.json'));
    invariant(existing.length < 256, 'INBOX_FULL', 'The inbox already contains 256 files.');
    const capacity = await fs.statfs(this.root), free = Number(capacity.bsize) * Number(capacity.bavail);
    let reserved = 0;
    for (const inbox of this.reservations.inboxes) {
      const entries = await inbox.list();
      invariant(!entries.some(entry => entry.status === 'unreadable'), 'RESERVATION_UNKNOWN', 'An unreadable upload must be reviewed before reserving more space.');
      reserved += entries.filter(entry => entry.status === 'uploading').reduce((sum, entry) => sum + Math.max(0, entry.bytes - entry.offset), 0);
    }
    invariant(Number.isSafeInteger(existingBytes) && existingBytes >= 0 && existingBytes <= bytes, 'UPLOAD_RESERVATION', 'Invalid staged file reservation.');
    invariant(Number.isSafeInteger(free) && free >= reserved + bytes - existingBytes + 16 * 1024 * 1024, 'NO_SPACE', 'Insufficient free space for this file, existing uploads and the safety reserve.');
    const id = this.id(), meta = { id, name, bytes, expectedSha256: sha256 ?? null };
    const part = await fs.open(this.file(id, '.part'), 'wx', 0o600);
    try { await part.sync(); } finally { await part.close(); }
    const metadata = await fs.open(this.file(id, '.json'), 'wx', 0o600);
    try { await metadata.writeFile(JSON.stringify(meta)); await metadata.sync(); } finally { await metadata.close(); }
    await this.syncDirectory();
    return { ...meta, offset: 0, status: 'uploading' };
    });
    this.reservations.tail = operation.catch(() => {}); return operation;
  }
  async meta(id) {
    const file = this.file(id, '.json'), stat = await fs.lstat(file);
    invariant(stat.isFile() && !stat.isSymbolicLink() && stat.size < 2048, 'UPLOAD_METADATA', 'Invalid upload metadata.');
    const meta = JSON.parse(await fs.readFile(file, 'utf8'));
    invariant(meta.id === id && Number.isSafeInteger(meta.bytes) && meta.bytes >= this.policy.minimum && meta.bytes <= this.policy.maximum &&
      typeof meta.name === 'string' && this.policy.names.test(meta.name) && (meta.expectedSha256 === null || /^[a-f0-9]{64}$/.test(meta.expectedSha256)),
      'UPLOAD_METADATA', 'Invalid upload metadata.');
    return meta;
  }
  async status(id) {
    const meta = await this.meta(id);
    for (const suffix of ['.payload', '.part']) {
      try {
        const stat = await fs.lstat(this.file(id, suffix));
        invariant(stat.isFile() && !stat.isSymbolicLink() && stat.size <= meta.bytes, 'UPLOAD_FILE', 'The upload has an invalid type or size.');
        invariant(suffix !== '.payload' || stat.size === meta.bytes, 'UPLOAD_FILE', 'A verified file changed size.');
        return { ...meta, offset: stat.size, status: suffix === '.payload' ? 'verified' : 'uploading' };
      } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    invariant(false, 'UPLOAD_MISSING', 'The upload file is missing.', { status: 404 });
  }
  async dataFile(id, status) {
    return this.file(id, status.status === 'verified' ? '.payload' : '.part');
  }
  async append(id, offset, data) {
    invariant(Number.isSafeInteger(offset) && offset >= 0 && data.length > 0 && data.length <= 1024 * 1024,
      'UPLOAD_RANGE', 'Invalid upload chunk.', { status: 400 });
    return this.exclusive(id, async () => {
      const status = await this.status(id);
      invariant(status.status === 'uploading' && offset === status.offset && offset + data.length <= status.bytes,
        'UPLOAD_OFFSET', 'Upload offset changed. Resume from the returned offset.');
      const file = await fs.open(this.file(id, '.part'), 'r+');
      try {
        let written = 0;
        while (written < data.length) {
          const result = await file.write(data, written, data.length - written, offset + written);
          invariant(result.bytesWritten > 0, 'SHORT_WRITE', 'File write made no progress.'); written += result.bytesWritten;
        }
        await file.sync();
      } finally { await file.close(); }
      return { id, offset: offset + data.length, status: 'uploading' };
    });
  }
  async verify(id, { signal, progress = async () => {} } = {}) {
    return this.exclusive(id, async () => {
      const status = await this.status(id);
      invariant(status.offset === status.bytes, 'UPLOAD_INCOMPLETE', 'Finish uploading the file first.');
      const file = await fs.open(await this.dataFile(id, status), 'r');
      const hash = createHash('sha256'), prefix = Buffer.alloc(Math.min(status.bytes, 16384));
      let info;
      try {
        const first = await file.read(prefix, 0, prefix.length, 0);
        invariant(first.bytesRead === prefix.length, 'SHORT_READ', 'Could not inspect the complete file header.');
        const before = await file.stat();
        info = await this.policy.inspect(file, prefix, status);
        // QuickJS hashes on the coordinator thread. Bound each synchronous
        // update so renderer heartbeats can run between verification chunks.
        const chunk = Buffer.alloc(64 * 1024);
        let yieldAt = Date.now(), progressAt = -Infinity;
        for (let position = 0; position < status.bytes;) {
          invariant(!signal?.aborted, 'UPLOAD_CANCELLED', 'File verification was cancelled; uploaded bytes were retained.');
          const { bytesRead } = await file.read(chunk, 0, Math.min(chunk.length, status.bytes - position), position);
          invariant(bytesRead > 0, 'SHORT_READ', 'File verification ended early.'); hash.update(chunk.subarray(0, bytesRead)); position += bytesRead;
          if (position === status.bytes || Date.now() - progressAt >= 250) {
            await progress({ completed: position, total: status.bytes, phase: 'Verifying file checksum' });
            progressAt = Date.now();
          }
          if (Date.now() - yieldAt >= 25) { await new Promise(resolve => setTimeout(resolve, 0)); yieldAt = Date.now(); }
        }
        const after = await file.stat();
        invariant(after.size === status.bytes && after.mtimeMs === before.mtimeMs, 'PAYLOAD_CHANGED', 'The file changed during verification.');
      } finally { await file.close(); }
      const sha256 = hash.digest('hex');
      invariant(!status.expectedSha256 || sha256 === status.expectedSha256, 'PAYLOAD_HASH_MISMATCH', 'File checksum differs from the expected value.');
      invariant(!signal?.aborted, 'UPLOAD_CANCELLED', 'File verification was cancelled; uploaded bytes were retained.');
      if (status.status !== 'verified') { await fs.rename(this.file(id, '.part'), this.file(id, '.payload')); await this.syncDirectory(); }
      // Always rehash before dispatch; this inbox entry is not an execution receipt.
      return { ...info, id, sha256, status: 'verified' };
    });
  }
  async list() {
    const result = [];
    for (const name of await fs.readdir(this.root)) {
      if (!/^[a-f0-9]{32}\.json$/.test(name)) continue;
      try { result.push(await this.status(name.slice(0, 32))); }
      catch { result.push({ id: name.slice(0, 32), status: 'unreadable', name: 'Incomplete inbox entry' }); }
    }
    return result;
  }
  async importFile(source, name, context = {}) {
    const stat = await fs.lstat(source);
    invariant(stat.isFile() && !stat.isSymbolicLink(), 'IMPORT_FILE', 'Only regular payload files can enter the inbox.');
    const input = await fs.open(source, 'r'); let entry;
    try {
      const before = await input.stat();
      entry = await this.begin({ name, bytes: before.size });
      const chunk = Buffer.alloc(1024 * 1024);
      for (let offset = 0; offset < before.size;) {
        invariant(!context.signal?.aborted, 'UPLOAD_CANCELLED', 'Copy cancelled. The original file was not changed.');
        const { bytesRead } = await input.read(chunk, 0, Math.min(chunk.length, before.size - offset), offset);
        invariant(bytesRead > 0, 'IMPORT_TRUNCATED', 'The source file changed while importing.');
        await this.append(entry.id, offset, chunk.subarray(0, bytesRead)); offset += bytesRead;
        await context.progress?.({ completed: offset, total: before.size, phase: 'Copying file to console storage' });
        await new Promise(resolve => setTimeout(resolve, 0));
      }
      const after = await input.stat();
      invariant(after.size === before.size && after.mtimeMs === before.mtimeMs,
        'IMPORT_CHANGED', 'The source file changed while importing.');
      return await this.verify(entry.id, context);
    } catch (error) {
      // Only this new, unexposed import is removed. The FTP source is preserved.
      if (entry) for (const suffix of ['.part', '.payload', '.json']) {
        try { await fs.unlink(this.file(entry.id, suffix)); } catch (removeError) { if (removeError.code !== 'ENOENT') break; }
      }
      throw error;
    } finally { await input.close(); }
  }
}
