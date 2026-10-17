// SPDX-License-Identifier: GPL-3.0-or-later
// CNT/FIH interpretation adapted from GronedWaffel PS Neighborhood's package
// inspector; its PS4 header reference is flatz/ps4_remote_pkg_installer/pkg.h.
// Structural checks and SHA-256 are not console signature/license validation.
import { invariant } from './errors.mjs';
export const MAX_PACKAGE = 1024 ** 4;
const HEADER = 0x438;
export async function inspectPackage(read, totalBytes) {
  invariant(Number.isSafeInteger(totalBytes) && totalBytes >= 0x2000 && totalBytes <= MAX_PACKAGE,
    'PACKAGE_SIZE', 'Choose a complete PKG between 8 KiB and 1 TiB.');
  async function header(at) {
    invariant(Number.isSafeInteger(at) && at >= 0 && at <= totalBytes - HEADER, 'PACKAGE_BOUNDS', 'Package header is outside the file.');
    const result = await read(at, HEADER);
    invariant(result.length === HEADER, 'PACKAGE_TRUNCATED', 'Could not read the complete package header.');
    return Buffer.from(result);
  }
  let bytes = await header(0), base = 0n, signing = 'unknown', format = 'cnt';
  if (bytes.readUInt32BE(0) === 0x7f464948) {
    invariant(bytes.readUInt16LE(6) === 3 && [0, 128].includes(bytes[5]), 'PACKAGE_FIH', 'Unsupported finalized-image header.');
    base = bytes.readBigUInt64LE(0x58);
    const start = bytes.readBigUInt64LE(0x10), size = bytes.readBigUInt64LE(0x18);
    invariant(start >= 0x100n && size > 0n && start + size <= base && base + BigInt(HEADER) <= BigInt(totalBytes),
      'PACKAGE_BOUNDS', 'The finalized-image ranges exceed the package file.');
    signing = bytes[5] === 0 ? 'debug' : 'retail'; format = 'fih'; bytes = await header(Number(base));
  }
  invariant(bytes.readUInt32BE(0) === 0x7f434e54, 'PACKAGE_MAGIC', 'This file has no supported CNT package header.');
  const content = bytes.subarray(0x40, 0x64);
  invariant([...content].every(byte => byte < 128), 'PACKAGE_CONTENT', 'Invalid package content identity.');
  const contentId = content.toString('ascii');
  invariant(/^[A-Z0-9]{6}-(?!NPXS)[A-Z]{4}\d{5}_00-[A-Z0-9]{16}$/.test(contentId), 'PACKAGE_CONTENT', 'Invalid game or homebrew content ID.');
  const titleId = contentId.slice(7, 16), ps5 = /^(PPSA|MOUU)/.test(titleId);
  const type = bytes.readUInt32BE(0x74), flags = bytes.readUInt32BE(0x78);
  invariant((ps5 ? [0x20, 0x21, 0x22, 0x26] : [0x1a, 0x1b, 0x1c, 0x1e]).includes(type),
    'PACKAGE_TYPE', 'Unsupported package content type.');
  if (ps5) {
    const count = bytes.readUInt32BE(0x10), table = BigInt(bytes.readUInt32BE(0x18));
    const body = bytes.readBigUInt64BE(0x20), length = bytes.readBigUInt64BE(0x28);
    invariant(count > 0 && count <= 4096 && table >= 0x80n && table + BigInt(count) * 32n <= body + length &&
      body >= 0x80n && length > 0n && base + body + length <= BigInt(totalBytes), 'PACKAGE_BOUNDS', 'Package metadata extends beyond the file.');
  } else {
    invariant(format === 'cnt' && bytes.readBigUInt64BE(0x430) === BigInt(totalBytes), 'PACKAGE_INCOMPLETE', 'The PS4 package is incomplete or split.');
  }
  const patch = ps5 ? !!(flags & 0x40100000) : type === 0x1e || !!(flags & 0x60100000);
  return { platform: ps5 ? 'ps5' : 'ps4', contentId, titleId, type, patch,
    kind: patch ? 'Patch' : (ps5 ? [0x21, 0x22] : [0x1b, 0x1c]).includes(type) ? 'Add-on' : 'Game / app',
    signing, format: (ps5 ? 'ps5-' : 'ps4-') + format, bytes: totalBytes,
    validation: 'Header ranges and file checksum; the console must still validate package content.' };
}
