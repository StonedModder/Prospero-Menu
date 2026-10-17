// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';

const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const validSymbol = symbol => /^[A-Za-z_][A-Za-z0-9_]*$/.test(symbol);

// An incbin path alone does not invalidate Zig's cached assembly object when its
// contents change. Put a digest in the assembly itself, outside the exported span.
export function embeddingAssembly(assets) {
  const names = new Set();
  const lines = ['.section .rodata,"a",@progbits'];
  for (const { symbol, path, bytes } of assets) {
    if (!validSymbol(symbol) || names.has(symbol)) throw Error('Invalid or duplicate embedding symbol');
    if (!/^[A-Za-z0-9_./-]+$/.test(path) || !Buffer.isBuffer(bytes) || !bytes.length)
      throw Error('Invalid embedding input');
    names.add(symbol);
    lines.push('.ascii "SHA256:' + sha256(bytes) + '"',
      '.global ' + symbol + '_start', '.global ' + symbol + '_end',
      symbol + '_start:', '.incbin "' + path + '"', symbol + '_end:');
  }
  lines.push('.section .note.GNU-stack,"",@progbits', '');
  return lines.join('\n');
}

export function verifyEmbeddedAsset(elf, symbol, expected) {
  const fail = reason => { throw Error('Embedded asset ' + symbol + ': ' + reason); };
  if (!validSymbol(symbol) || !Buffer.isBuffer(elf) || !Buffer.isBuffer(expected) || !expected.length)
    fail('invalid verification input');
  function range(offset, length) {
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) || offset < 0 || length < 0 ||
        offset > elf.length || length > elf.length - offset) fail('truncated ELF data');
  }
  function u64(offset) {
    range(offset, 8);
    const value = elf.readBigUInt64LE(offset);
    if (value > BigInt(Number.MAX_SAFE_INTEGER)) fail('ELF integer out of bounds');
    return Number(value);
  }
  range(0, 64);
  if (elf.subarray(0, 7).toString('hex') !== '7f454c46020101' || elf.readUInt16LE(18) !== 62 ||
      ![2, 3].includes(elf.readUInt16LE(16))) fail('expected a linked x86-64 ELF');
  const shoff = u64(40), shsize = elf.readUInt16LE(58), shcount = elf.readUInt16LE(60);
  const phoff = u64(32), phsize = elf.readUInt16LE(54), phcount = elf.readUInt16LE(56);
  if (shsize !== 64 || !shcount || phsize !== 56 || !phcount) fail('missing ELF tables');
  range(shoff, shsize * shcount); range(phoff, phsize * phcount);
  const sections = Array.from({ length: shcount }, (_, index) => {
    const at = shoff + index * shsize;
    return { type: elf.readUInt32LE(at + 4), flags: u64(at + 8), address: u64(at + 16),
      offset: u64(at + 24), size: u64(at + 32), link: elf.readUInt32LE(at + 40), stride: u64(at + 56) };
  });
  const symbols = new Map();
  for (const section of sections.filter(s => s.type === 2)) {
    if (section.stride !== 24 || section.size % 24) fail('invalid symbol table');
    range(section.offset, section.size);
    const strings = sections[section.link];
    if (!strings || strings.type !== 3) fail('missing symbol strings');
    range(strings.offset, strings.size);
    for (let at = section.offset; at < section.offset + section.size; at += 24) {
      const nameAt = elf.readUInt32LE(at), index = elf.readUInt16LE(at + 6);
      if (!index) continue; // Undefined references are not asset definitions.
      if (nameAt >= strings.size) fail('invalid symbol name');
      const end = elf.indexOf(0, strings.offset + nameAt);
      if (end < 0 || end >= strings.offset + strings.size) fail('unterminated symbol name');
      const name = elf.toString('utf8', strings.offset + nameAt, end);
      if (name !== symbol + '_start' && name !== symbol + '_end') continue;
      if (symbols.has(name)) fail('ambiguous symbol ' + name);
      symbols.set(name, { address: u64(at + 8), index });
    }
  }
  const start = symbols.get(symbol + '_start'), end = symbols.get(symbol + '_end');
  if (!start || !end) fail('missing span symbols');
  if (end.address - start.address !== expected.length || start.index !== end.index)
    fail('span length or section mismatch');
  const section = sections[start.index];
  if (!section || section.type !== 1 || !(section.flags & 2)) fail('span is not allocated file data');
  const relative = start.address - section.address;
  if (relative < 0 || relative > section.size || expected.length > section.size - relative)
    fail('span extends outside its section');
  const offset = section.offset + relative;
  range(offset, expected.length);
  const mappings = [];
  for (let index = 0; index < phcount; index++) {
    const at = phoff + index * phsize;
    if (elf.readUInt32LE(at) !== 1) continue;
    const fileAt = u64(at + 8), address = u64(at + 16), fileSize = u64(at + 32), memorySize = u64(at + 40);
    range(fileAt, fileSize);
    if (fileSize > memorySize) fail('invalid load segment');
    const within = start.address - address;
    if (within >= 0 && within <= fileSize && expected.length <= fileSize - within)
      mappings.push(fileAt + within);
  }
  if (mappings.length !== 1 || mappings[0] !== offset) fail('span has no unique file-backed load mapping');
  if (!elf.subarray(offset, offset + expected.length).equals(expected)) fail('bytes differ from current build');
  return { symbol, offset, bytes: expected.length, sha256: sha256(expected) };
}
