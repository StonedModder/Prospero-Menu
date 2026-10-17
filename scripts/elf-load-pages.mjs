// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 loaders protect 16 KiB pages, not individual ELF sections. Generated
// payloads must not put text and a following data LOAD in the same page.
export function verifyLoadPages(bytes) {
  const b = Buffer.from(bytes);
  if (b.length < 64 || b.readUInt32LE(0) !== 0x464c457f || b[4] !== 2 || b[5] !== 1)
    throw new Error('Expected a little-endian ELF64 payload');
  const offset = b.readBigUInt64LE(32), stride = b.readUInt16LE(54), count = b.readUInt16LE(56);
  if (stride !== 56 || !count || offset + BigInt(stride * count) > BigInt(b.length))
    throw new Error('Invalid ELF program header table');
  const loads = [];
  for (let i = 0; i < count; i++) {
    const at = Number(offset) + i * stride;
    if (b.readUInt32LE(at) !== 1) continue;
    const file = b.readBigUInt64LE(at + 8), start = b.readBigUInt64LE(at + 16);
    const size = b.readBigUInt64LE(at + 32), memory = b.readBigUInt64LE(at + 40);
    if (!memory) continue;
    if (size > memory || file + size > BigInt(b.length)) throw new Error('Invalid LOAD extent');
    if (start % 16384n || file % 16384n) throw new Error('LOAD segment must start on a separate 16 KiB page');
    loads.push({ start, end: (start + memory + 16383n) & ~16383n });
  }
  if (!loads.length) throw new Error('Payload has no LOAD segments');
  loads.sort((a, b) => a.start < b.start ? -1 : a.start > b.start ? 1 : 0);
  for (let i = 1; i < loads.length; i++)
    if (loads[i].start < loads[i - 1].end) throw new Error('LOAD segments share a protection page');
  return loads.length;
}
