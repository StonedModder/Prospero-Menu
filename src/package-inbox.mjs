// SPDX-License-Identifier: GPL-3.0-or-later
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { PayloadInbox } from './inbox.mjs';
import { inspectPackage, MAX_PACKAGE } from './package-format.mjs';
import { invariant } from './errors.mjs';
export async function inspectPackageFile(file, bytes, hash = null) {
  return inspectPackage(async (position, length) => {
    const buffer = Buffer.alloc(length); let offset = 0;
    while (offset < length) {
      const result = await file.read(buffer, offset, length - offset, position + offset);
      invariant(result.bytesRead > 0, 'PACKAGE_TRUNCATED', 'Package header ended early.'); offset += result.bytesRead;
    }
    hash?.update(buffer);return buffer;
  }, bytes);
}
export class PackageInbox extends PayloadInbox {
  constructor(root, { id, reservationGroup }) {
    super(root, { id, reservationGroup, policy: { minimum: 0x2000, maximum: MAX_PACKAGE, names: /^[^/\\\x00-\x1f]{1,180}\.pkg$/i,
      nameMessage: 'Choose a complete .pkg file.', sizeMessage: 'Packages must be between 8 KiB and 1 TiB.',
      inspect: (file, _prefix, status) => inspectPackageFile(file, status.bytes) } });
  }
  // Storage selections retain only a reference. The installer streams from
  // the original drive; there is no .part/.payload copy or size reservation.
  async referenceFile(source, name, context = {}) {
    invariant(this.policy.names.test(name), 'PAYLOAD_NAME', this.policy.nameMessage);
    const before = await fs.lstat(source);
    invariant(before.isFile() && !before.isSymbolicLink() && before.size >= this.policy.minimum && before.size <= this.policy.maximum,
      'PACKAGE_SOURCE', 'Choose a complete regular PKG file.');
    invariant((await fs.readdir(this.root)).filter(n => n.endsWith('.json')).length < 256, 'INBOX_FULL', 'Remove an old package entry first.');
    const id = this.id(), meta = { id, name, bytes: before.size, expectedSha256: null, source,
      sourceIdentity: { dev: before.dev, ino: before.ino, modified: before.mtimeMs } };
    try {
      for (const [suffix, text] of [['.source', source], ['.json', JSON.stringify(meta)]]) {
        const file = await fs.open(this.file(id, suffix), 'wx', 0o600);
        try { await file.writeFile(text); await file.sync(); } finally { await file.close(); }
      }
      await this.syncDirectory();
      // Local PKGs are streamed from their original descriptor by the native
      // installer. Bind the inspected headers, not a second full-file scan on
      // the menu's coordinator thread. Downloads retain full SHA verification.
      const status = await this.status(id), file = await fs.open(await this.dataFile(id, status), 'r');
      let result;
      try {
        invariant(!context.signal?.aborted, 'UPLOAD_CANCELLED', 'Package selection cancelled.');
        const hash = createHash('sha256'), info = await inspectPackageFile(file, status.bytes, hash);
        await this.dataFile(id, status);
        result = {...info,id,sha256:null,headerSha256:hash.digest('hex'),verification:'header',status:'verified',
          validation:'Package headers and source identity checked; the console validates content during installation.'};
      } finally { await file.close(); }
      return { ...result, source };
    } catch (error) {
      for (const suffix of ['.source', '.json']) await fs.unlink(this.file(id, suffix)).catch(() => {});
      throw error;
    }
  }
  async status(id) {
    const meta = await this.meta(id);
    if (!meta.source) return super.status(id);
    invariant(typeof meta.source === 'string' && path.isAbsolute(meta.source) && meta.source.length < 1024 &&
      !/[\x00-\x1f]/.test(meta.source) && meta.sourceIdentity, 'PACKAGE_SOURCE', 'Invalid package source reference.');
    // Do not make a disconnected USB an unreadable inbox entry that blocks
    // unrelated downloads. Actual source access is checked before use.
    return { ...meta, offset: meta.bytes, status: 'verified', external: true };
  }
  async dataFile(id, status) {
    if (!status.external) return super.dataFile(id, status);
    const named = await fs.readFile(this.file(id, '.source'), 'utf8');
    invariant(named === status.source, 'PACKAGE_SOURCE', 'Package source reference changed. Select the file again.');
    let current = named.startsWith('/data/') ? '/data' : named.match(/^\/mnt\/usb[0-7]\//)?.[0].slice(0, -1) || path.parse(named).root;
    for (const piece of ['', ...named.slice(current.length).split(path.sep).filter(Boolean)]) {
      if (piece) current = path.join(current, piece);
      let stat;
      try { stat = await fs.lstat(current); }
      catch { invariant(false, 'PACKAGE_SOURCE_MISSING', 'Package source is unavailable. Reconnect the USB drive or choose the file again.'); }
      invariant(!stat.isSymbolicLink(), 'PACKAGE_SOURCE', 'Package source cannot contain symbolic links.');
    }
    const stat = await fs.lstat(named), identity = status.sourceIdentity;
    invariant(stat.isFile() && stat.size === status.bytes && stat.dev === identity.dev && stat.ino === identity.ino && stat.mtimeMs === identity.modified,
      'PACKAGE_SOURCE_CHANGED', 'Package source changed. Choose the file again.');
    return named;
  }
  // A multipart Content-Length includes form headers. Reserve its upper bound,
  // then seal the actual file length only after the complete form was accepted.
  async sealReceived(id, bytes, sha256) {
    return this.exclusive(id, async () => {
      const status = await this.status(id);
      invariant(status.status === 'uploading' && Number.isSafeInteger(bytes) && bytes >= this.policy.minimum &&
        bytes === status.offset && bytes <= status.bytes && /^[a-f0-9]{64}$/.test(sha256),
        'PACKAGE_RECEIVE', 'The received package changed or is incomplete.');
      const meta = { id, name: status.name, bytes, expectedSha256: sha256 }, next = this.file(id,'.json.next');
      const out = await fs.open(next,'wx',0o600);
      try { await out.writeFile(JSON.stringify(meta)); await out.sync(); } finally { await out.close(); }
      await fs.rename(next,this.file(id,'.json')); await this.syncDirectory();return this.status(id);
    });
  }
  async discardReceived(id) {
    return this.exclusive(id, async () => {
      const status = await this.status(id);
      invariant(status.status === 'uploading','PACKAGE_RECEIVE','A verified package cannot be discarded as an interrupted upload.');
      await fs.unlink(this.file(id,'.part'));await fs.unlink(this.file(id,'.json'));await this.syncDirectory();
    });
  }
  // Native downloads are already in ProsperoMenu's staging area on /data. Move the
  // owned file into the inbox instead of requiring another complete PKG copy.
  async adoptDownload(source, name, context) {
    const before = await fs.lstat(source), destination = await fs.stat(this.root);
    invariant(before.isFile() && !before.isSymbolicLink() && before.dev !== undefined && before.ino !== undefined && before.dev === destination.dev,
      'PACKAGE_STAGING', 'Package download staging must be a regular file on the inbox volume.');
    const entry = await this.allocate({name,bytes:before.size},before.size);
    try {
      await this.exclusive(entry.id,async()=>{
        const after=await fs.lstat(source),empty=await this.status(entry.id);
        invariant(after.isFile() && !after.isSymbolicLink() && after.dev===before.dev && after.ino===before.ino &&
          after.size===before.size && after.mtimeMs===before.mtimeMs && empty.status==='uploading' && empty.offset===0,
          'PACKAGE_STAGING','Package download changed before staging.');
        await fs.rename(source,this.file(entry.id,'.part'));await this.syncDirectory();
      });
      return await this.verify(entry.id,context);
    } catch(error) {
      // A failed verification remains a visible staged package for inspection.
      // Never discard the original file or promote it as verified.
      error.packageId=entry.id;throw error;
    }
  }
  async describe(id) {
    const status = await this.status(id);
    if (status.status !== 'verified') return status;
    const file = await fs.open(await this.dataFile(id, status), 'r');
    try { return { ...status, package: await inspectPackageFile(file, status.bytes) }; }
    finally { await file.close(); }
  }
}
export async function installPackageInbox(runtime, root) {
  runtime.packageInbox = await new PackageInbox(root, { id: runtime.id, reservationGroup: runtime.inbox?.reservations }).init();
  runtime.packageItems = await runtime.packageInbox.list();
  runtime.jobs.register('package.verify', {
    async validate(input) {
      const file = await runtime.packageInbox.status(input?.id);
      invariant(file.offset === file.bytes, 'UPLOAD_INCOMPLETE', 'Complete the package upload before verification.');
      return { id: file.id, bytes: file.bytes, name: file.name };
    },
    resources: input => ['package-file:' + input.id, 'file-verification'], timeout: 48 * 60 * 60 * 1000, cancellable: true,
    async run(input, context) {
      try {
        const result = await runtime.packageInbox.verify(input.id, context);
        return { completed: true, id: input.id, sha256: result.sha256, package: result,
          message: 'Package uploaded and verified. Installation has not started.' };
      } catch (error) {
        if (error.code === 'UPLOAD_CANCELLED') return { cancelled: true, message: 'Verification cancelled; uploaded bytes retained.' };
        error.noSideEffects = true; throw error;
      } finally { runtime.packageItems = await runtime.packageInbox.list(); runtime.sequence++; }
    },
    async verify(result, input) { return result.id === input.id && (await runtime.packageInbox.status(input.id)).status === 'verified'; },
    async reconcile(job) {
      // A new read-only checksum may establish verification after an interrupted
      // job. It does not submit an installation or touch an installed title.
      const result = await runtime.packageInbox.verify(job.args.id);
      runtime.packageItems = await runtime.packageInbox.list();
      return { completed: true, id: job.args.id, sha256: result.sha256, package: result, message: 'Package verification confirmed; no installation was submitted.' };
    }
  });
  return runtime.packageInbox;
}
