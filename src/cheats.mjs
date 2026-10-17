// SPDX-License-Identifier: GPL-3.0-or-later
// JSON field compatibility follows etaHEN CheatManager's public JSON format.
// Advanced section/code-cave relocation is outside ProsperoMenu's requested scope.
import { invariant, PmError } from './errors.mjs';
import { requireGame } from './firmware.mjs';
const titlePattern = /^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/;
const versionPattern = /^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/;
function text(value, maximum, field) {
  invariant(typeof value === 'string' && value.length > 0 && value.length <= maximum && !/[\x00-\x08\x0b-\x1f]/.test(value),
    'CHEAT_FORMAT', `Invalid cheat ${field}.`); return value;
}
function hex(value, field) {
  invariant(typeof value === 'string' && /^(?:[a-fA-F0-9]{2})+$/.test(value) && value.length <= 8192,
    'CHEAT_BYTES', `Invalid ${field} patch bytes.`); return value.toLowerCase();
}
export function importCheatJson(source, identity = {}) {
  invariant(typeof source === 'string' && source.length <= 4 * 1024 * 1024, 'CHEAT_SIZE', 'Cheat document is too large.');
  let document;
  try { document = JSON.parse(source); } catch { throw new PmError('CHEAT_JSON', 'The cheat file is not valid JSON.'); }
  invariant(document && !Array.isArray(document), 'CHEAT_FORMAT', 'Cheat document must be an object.');
  const titleId = identity.titleId ?? document.id ?? identity.fallback?.titleId, version = identity.version ?? document.version ?? identity.fallback?.version;
  invariant(titlePattern.test(titleId) && versionPattern.test(version), 'CHEAT_IDENTITY', 'This file needs a title ID and version. Start its target game if the file does not include them.');
  if (document.id != null) invariant(document.id === titleId, 'CHEAT_IDENTITY', 'Cheat title ID differs from the selected game.');
  if (document.version != null) invariant(document.version === version, 'CHEAT_IDENTITY', 'Cheat version differs from the selected game.');
  const name = text(document.name, 200, 'name'), process = text(document.process, 128, 'process');
  invariant(!/[/\\]/.test(process), 'CHEAT_PROCESS', 'Cheat process must be a module name.');
  invariant(Array.isArray(document.mods) && document.mods.length > 0 && document.mods.length <= 1024, 'CHEAT_FORMAT', 'Invalid cheat list.');
  let totalBytes = 0;
  const cheats = document.mods.map((mod, index) => {
    invariant(mod && Array.isArray(mod.memory) && mod.memory.length > 0 && mod.memory.length <= 256, 'CHEAT_FORMAT', 'Invalid cheat memory patches.');
    const memory = mod.memory.map(patch => {
      invariant(patch && typeof patch.offset === 'string' && /^(?:0x)?[0-9a-fA-F]{1,16}$/.test(patch.offset),
        'CHEAT_OFFSET', 'Invalid module-relative patch offset.');
      invariant(patch.absolute == null || typeof patch.absolute === 'boolean', 'CHEAT_ADDRESS_MODE', 'Absolute addressing must be an explicit boolean.');
      invariant(!patch.codeCaveReloc, 'CHEAT_ADDRESS_MODE', 'Code-cave relocation is not supported by ProsperoMenu.');
      const offset = BigInt(patch.offset.startsWith('0x') ? patch.offset : '0x' + patch.offset);
      const on = hex(patch.on, 'enabled'), off = hex(patch.off, 'disabled');
      const section = patch.section == null || patch.section === '' ? 0 : Number(patch.section);
      invariant(Number.isSafeInteger(section) && section >= 0 && section < 16, 'CHEAT_SECTION', 'Invalid module section.');
      totalBytes += Math.max(on.length, off.length) / 2;
      return { offset: '0x' + offset.toString(16), section, on, off, ...(patch.absolute ? { absolute: true } : {}) };
    });
    invariant(totalBytes <= 1024 * 1024, 'CHEAT_SIZE', 'Cheat patches exceed the total size limit.');
    return { id: String(index), name: text(mod.name, 200, 'name'), description: typeof mod.description === 'string' ? mod.description.slice(0, 2000) : '', memory };
  });
  return { schema: 1, titleId, version, name, process, credits: Array.isArray(document.credits) ? document.credits.filter(x => typeof x === 'string').map(x => x.slice(0, 200)).slice(0, 32) : [], cheats };
}
function sameGame(a, b) {
  return a && b && ['pid', 'generation', 'titleId', 'version'].every(key => a[key] === b[key]);
}
export function cheatPacket(pack, cheatId, enabled) {
  invariant(typeof enabled === 'boolean', 'CHEAT_STATE', 'Choose enabled or disabled.');
  const cheat = pack.cheats.find(item => item.id === cheatId);
  invariant(cheat, 'CHEAT_MISSING', 'Cheat was not found.');
  const patches = cheat.memory.map(item => {
    invariant(item.absolute || item.section === 0, 'CHEAT_SECTION_UNSUPPORTED', 'Advanced section addressing is not supported by ProsperoMenu.');
    return { absolute: Boolean(item.absolute), offset: BigInt(item.offset), before: Buffer.from(enabled ? item.off : item.on, 'hex'), after: Buffer.from(enabled ? item.on : item.off, 'hex') };
  }).sort((a, b) => Number(a.absolute) - Number(b.absolute) || (a.offset < b.offset ? -1 : a.offset > b.offset ? 1 : 0));
  invariant(patches.every(p => p.offset >= 0n && p.offset <= 0xffffffffffffffffn - BigInt(Math.max(p.before.length, p.after.length))), 'PATCH_BOUNDS', 'A patch offset overflows the native address width.');
  invariant(patches.every(p => !p.absolute || p.offset >= 0x10000n && p.offset + BigInt(Math.max(p.before.length, p.after.length)) <= 0x800000000000n),
    'PATCH_BOUNDS', 'An absolute patch must stay within user process address space.');
  invariant(patches.every((p, i) => !i || patches[i - 1].absolute !== p.absolute || patches[i - 1].offset + BigInt(Math.max(patches[i - 1].before.length, patches[i - 1].after.length)) <= p.offset),
    'PATCH_OVERLAP', 'This cheat contains overlapping patches.');
  const unequal = patches.some(p => p.before.length !== p.after.length), header = unequal ? 20 : 16;
  const bytes = 16 + patches.reduce((sum, p) => sum + header + p.before.length + p.after.length, 0), packet = Buffer.alloc(bytes);
  packet.writeUInt32LE(0x54435031, 0); packet.writeUInt32LE(unequal ? 3 : patches.some(p => p.absolute) ? 2 : 1, 4); packet.writeUInt32LE(patches.length, 8); packet.writeUInt32LE(bytes, 12);
  let at = 16;
  for (const patch of patches) {
    const view = new DataView(packet.buffer, packet.byteOffset + at, header);
    view.setBigUint64(0, patch.offset, true); view.setUint32(8, patch.before.length, true); view.setUint32(unequal ? 16 : 12, patch.absolute ? 1 : 0, true);
    if (unequal) view.setUint32(12, patch.after.length, true);
    patch.before.copy(packet, at + header); patch.after.copy(packet, at + header + patch.before.length); at += header + patch.before.length + patch.after.length;
  }
  return packet;
}
export class CheatSession {
  constructor(adapter) { this.adapter = adapter; this.busy = false; this.blocked = false; }
  async set(game, pack, cheatId, enabled) {
    requireGame(game);
    invariant(typeof enabled === 'boolean', 'CHEAT_STATE', 'Cheat state must be enabled or disabled.');
    invariant(!this.busy && !this.blocked, 'CHEAT_BUSY', 'Cheat execution is busy or requires recovery.');
    invariant(game.titleId === pack.titleId && game.version === pack.version, 'CHEAT_MISMATCH',
      'These cheats target a different title or game version.');
    const cheat = pack.cheats.find(item => item.id === cheatId);
    invariant(cheat, 'CHEAT_MISSING', 'Cheat was not found.');
    this.busy = true; let lease, outcome, failure; const written = [];
    try {
      invariant(sameGame(await this.adapter.identity(), game), 'GAME_CHANGED', 'The running game changed.');
      lease = await this.adapter.acquire(game);
      invariant(lease?.stopped === true && sameGame(await lease.identity(), game), 'GAME_NOT_STOPPED', 'Could not obtain a verified stopped game instance.');
      const patches = [];
      for (const memory of cheat.memory) {
        invariant(!memory.absolute || typeof lease.absolute === 'function', 'CHEAT_ADDRESS_MODE', 'The memory adapter does not implement absolute range validation.');
        const span = Math.max(memory.on.length, memory.off.length) / 2;
        const section = memory.absolute ? await lease.absolute(pack.process, BigInt(memory.offset), span) : await lease.section(pack.process, memory.section);
        invariant(section && section.readable === true && section.patchable === true, 'SECTION_UNAVAILABLE', 'The target module section is not safely patchable.');
        const address = (memory.absolute ? 0n : BigInt(section.base)) + BigInt(memory.offset), bytes = Buffer.from(enabled ? memory.on : memory.off, 'hex');
        const expected = Buffer.from(enabled ? memory.off : memory.on, 'hex');
        invariant(!memory.absolute || address >= 0x10000n && address + BigInt(span) <= 0x800000000000n, 'PATCH_BOUNDS', 'Absolute address is outside user process memory.');
        invariant(address >= BigInt(section.base) && address + BigInt(span) <= BigInt(section.base) + BigInt(section.size),
          'PATCH_BOUNDS', 'A cheat patch extends outside its module section.');
        const original = Buffer.from(await lease.read(address, span));
        invariant(original.length === span && original.subarray(0, expected.length).equals(expected), 'PATCH_MISMATCH',
          'Game bytes differ from this cheat’s expected state.', { action: 'Check the game version and conflicting mods. No changes were applied.' });
        patches.push({ address, bytes, span, original: original.subarray(0, bytes.length) });
      }
      patches.sort((a, b) => a.address < b.address ? -1 : 1);
      invariant(patches.every((patch, i) => !i || patches[i - 1].address + BigInt(patches[i - 1].span) <= patch.address),
        'PATCH_OVERLAP', 'This cheat contains overlapping patches.');
      invariant(sameGame(await lease.identity(), game), 'GAME_CHANGED', 'The game instance changed before patching.');
      for (const patch of patches) {
        // Include the in-flight write in rollback: a failed write can be partial.
        written.push(patch); await lease.write(patch.address, patch.bytes);
        invariant(Buffer.from(await lease.read(patch.address, patch.bytes.length)).equals(patch.bytes), 'PATCH_VERIFY', 'A patch write could not be verified.');
      }
      await lease.flush(patches.map(patch => ({ address: patch.address, length: patch.bytes.length })));
      outcome = { completed: true, cheatId, enabled, game: { ...game }, patches: patches.length };
    } catch (error) {
      failure = error;
      if (written.length && lease) {
        try {
          invariant(sameGame(await lease.identity(), game), 'GAME_CHANGED', 'Cannot roll back into a different game instance.');
          for (const patch of written.reverse()) {
            await lease.write(patch.address, patch.original);
            invariant(Buffer.from(await lease.read(patch.address, patch.original.length)).equals(patch.original), 'ROLLBACK_VERIFY', 'Rollback verification failed.');
          }
          await lease.flush(written.map(patch => ({ address: patch.address, length: patch.original.length })));
        } catch {
          this.blocked = true; failure = new PmError('PATCH_RECOVERY_REQUIRED', 'The patch could not be completely rolled back.',
            { action: 'Close the game before applying further cheats.' });
        }
      }
    } finally {
      if (lease) {
        try { invariant(await lease.release() === true, 'RESUME_FAILED', 'The game did not confirm resuming.'); }
        catch { this.blocked = true; failure = new PmError('RESUME_FAILED', 'The game did not confirm resuming.',
          { action: 'Check the console before making another change.' }); }
      }
      this.busy = false;
    }
    if (failure) throw failure;
    return outcome;
  }
}
