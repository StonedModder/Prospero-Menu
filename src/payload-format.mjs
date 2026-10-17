// SPDX-License-Identifier: GPL-3.0-or-later
// Container layout follows etaHEN/etaHEN-Plugins make_plugin.py and PS5 ABI.
// This is structural validation, not a claim of safe executable code.
import { invariant } from './errors.mjs';
export const MAX_PAYLOAD = 128 * 1024 * 1024;
export function inspectPayload(input, name, { totalBytes = input.length } = {}) {
  const bytes = Buffer.from(input);
  invariant(typeof name === 'string' && /^[^/\\\x00-\x1f]{1,180}\.(elf|plugin|prx|sprx)$/i.test(name),
    'PAYLOAD_NAME', 'Choose an ELF, .plugin, .prx or .sprx file.');
  invariant(Number.isSafeInteger(totalBytes) && totalBytes >= 64 && totalBytes <= MAX_PAYLOAD && bytes.length >= 64 && bytes.length <= totalBytes,
    'PAYLOAD_SIZE', 'Payload size must be between 64 bytes and 128 MiB.');
  const extension = name.slice(name.lastIndexOf('.') + 1).toLowerCase();
  let offset = 0, pluginId = null, pluginVersion = null, platform = null, container = 'elf', containerBytes = totalBytes;
  if (extension === 'plugin') {
    const magic = Buffer.from('etaHEN_PLUGIN\0'), titleAt = magic.length, versionAt = titleAt + 10;
    invariant(bytes.subarray(0, magic.length).equals(magic) && bytes[titleAt + 9] === 0 && bytes[versionAt + 4] === 0,
      'PLUGIN_HEADER', 'The etaHEN plugin header is invalid.');
    pluginId = bytes.subarray(titleAt, titleAt + 9).toString('ascii');
    pluginVersion = bytes.subarray(versionAt, versionAt + 4).toString('ascii');
    invariant(/^[A-Za-z]{4}\d{5}$/.test(pluginId) && /^\d\.\d{2}$/.test(pluginVersion),
      'PLUGIN_HEADER', 'Invalid plugin title or version.');
    offset = versionAt + 5; container = 'plugin';
  }
  if (extension === 'prx' || extension === 'sprx') {
    const magic = bytes.readUInt32LE(0);
    if (magic === 0x1d3d154f || magic === 0xeef51454) {
      // Development FSELFs on PS5 also use 0x1d3d154f. That shared wrapper
      // cannot identify a PS4 target; bind unknown modules to the selected
      // game, just as with raw PRXs. Keep the PS5-specific wrapper check.
      platform = magic === 0xeef51454 ? 'ps5' : null; container = 'self';
      const count = bytes.readUInt16LE(24); offset = 32 + count * 32;
      invariant(count > 0 && count <= 128 && offset <= bytes.length - 64,
        'SELF_HEADER', `SELF entry table is invalid or incomplete (${count} entries).`);
      const declared = bytes.readBigUInt64LE(16), actual = BigInt(totalBytes);
      // Some tools declare the final 16-byte alignment without writing padding;
      // existing PRXs may also retain trailing data outside the SELF image.
      // Neither permits missing segment bytes or using a trailer as a header.
      invariant(declared >= BigInt(offset + 64) && (declared <= actual || declared === (actual + 15n) / 16n * 16n),
        'SELF_HEADER', `SELF size mismatch: header declares ${declared} bytes; file contains ${totalBytes}.`);
      containerBytes = Number(declared < actual ? declared : actual);
      for (let i = 0; i < count; i++) {
        const at = 32 + i * 32, start = bytes.readBigUInt64LE(at + 8), size = bytes.readBigUInt64LE(at + 16);
        invariant(start + size <= BigInt(containerBytes), 'SELF_BOUNDS', 'A SELF segment lies outside the available container data.');
      }
    }
  }
  invariant(bytes.length >= offset + 64 && bytes.subarray(offset, offset + 7).equals(Buffer.from([0x7f, 69, 76, 70, 2, 1, 1])),
    'ELF_HEADER', 'Expected a little-endian 64-bit ELF.');
  const elf = bytes.subarray(offset, Math.min(bytes.length, containerBytes)), u64 = at => elf.readBigUInt64LE(at);
  const type = elf.readUInt16LE(16), count = elf.readUInt16LE(56), ph = u64(32);
  const module = extension === 'prx' || extension === 'sprx';
  invariant(elf.readUInt16LE(18) === 62 && elf.readUInt16LE(52) === 64 && elf.readUInt16LE(54) === 56 &&
    count > 0 && count <= 128 && ph >= 64n && ph + BigInt(count * 56) <= BigInt(elf.length),
  'ELF_LAYOUT', 'Invalid x86-64 ELF program headers.');
  invariant(module ? type === 0xfe18 : type === 2 || type === 3, 'ELF_TYPE',
    module ? 'A PRX must be a native dynamic module; renaming an ELF does not convert it.' : 'Expected an executable payload.');
  let executable = false, entryValid = false, dynamic = false, memoryBytes = 0n;
  const ranges = [];
  for (let i = 0; i < count; i++) {
    const at = Number(ph) + i * 56, kind = elf.readUInt32LE(at), flags = elf.readUInt32LE(at + 4);
    if (kind === 2 || kind === 0x61000000) dynamic = true;
    if (kind !== 1) continue;
    const start = u64(at + 8), address = u64(at + 16), fileSize = u64(at + 32), memorySize = u64(at + 40);
    invariant(fileSize <= memorySize && address + memorySize <= 0xffffffffffffffffn &&
      (container === 'self' || start + fileSize <= BigInt(totalBytes - offset)), 'ELF_BOUNDS', 'Invalid payload segment bounds.');
    const alignment = u64(at + 48);
    invariant(alignment <= 1n || ((alignment & (alignment - 1n)) === 0n && address % alignment === start % alignment),
      'ELF_ALIGNMENT', 'Invalid payload segment alignment.');
    invariant(memorySize <= 512n * 1024n * 1024n, 'ELF_ALLOCATION', 'Payload requests an excessive segment allocation.');
    memoryBytes += memorySize;
    if (memorySize) ranges.push([address, address + memorySize]);
    if (flags & 1) { executable = true; if (u64(24) >= address && u64(24) < address + fileSize) entryValid = true; }
  }
  ranges.sort((a, b) => a[0] < b[0] ? -1 : 1);
  invariant(ranges.every((range, i) => !i || ranges[i - 1][1] <= range[0]), 'ELF_OVERLAP', 'Payload memory segments overlap.');
  invariant(memoryBytes <= 512n * 1024n * 1024n && executable && (module ? dynamic : entryValid),
    'ELF_ENTRY', module ? 'PRX lacks executable or dynamic metadata.' : 'ELF entry point is not within an executable segment.');
  return { name, format: extension, container, platform, pluginId, pluginVersion,
    bytes: totalBytes, memoryBytes: Number(memoryBytes), entry: '0x' + u64(24).toString(16),
    compatibility: module ? 'Requires matching game, platform, version and imports.' : 'Structural validation only; runtime compatibility is separate.' };
}
