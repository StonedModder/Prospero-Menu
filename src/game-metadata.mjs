// SPDX-License-Identifier: GPL-3.0-or-later
import { createHash } from 'node:crypto';
import { invariant, PmError } from './errors.mjs';
export const GAME_TITLE = /^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/;
export const GAME_VERSION = /^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/;
function string(bytes) {
  const zero = bytes.indexOf(0), content = zero < 0 ? bytes : bytes.subarray(0, zero);
  invariant(zero < 0 || bytes.subarray(zero).every(value => value === 0), 'GAME_METADATA_STRING', 'Metadata contains an embedded string terminator.');
  const value = content.toString('utf8');
  invariant(Buffer.from(value).equals(content), 'GAME_METADATA_STRING', 'Metadata text is not valid UTF-8.'); return value;
}
export function readSfo(data) {
  const bytes = Buffer.from(data);
  invariant(bytes.length >= 20 && bytes.length <= 1024 * 1024 && bytes.readUInt32LE(0) === 0x46535000 && bytes.readUInt32LE(4) === 0x101,
    'GAME_SFO', 'Unsupported or truncated PARAM.SFO.');
  const keys = bytes.readUInt32LE(8), values = bytes.readUInt32LE(12), count = bytes.readUInt32LE(16), result = Object.create(null), spans = [];
  invariant(count > 0 && count <= 1024 && 20 + count * 16 <= keys && keys < values && values <= bytes.length,
    'GAME_SFO_BOUNDS', 'SFO tables exceed the metadata file.');
  for (let i = 0; i < count; i++) {
    const at = 20 + i * 16, key = keys + bytes.readUInt16LE(at), format = bytes.readUInt16LE(at + 2),
      size = bytes.readUInt32LE(at + 4), capacity = bytes.readUInt32LE(at + 8), start = values + bytes.readUInt32LE(at + 12);
    invariant(key >= keys && key < values && size <= capacity && start + capacity <= bytes.length,
      'GAME_SFO_BOUNDS', 'SFO field lies outside the metadata file.');
    const end = bytes.indexOf(0, key);
    invariant(end > key && end < values && end - key <= 128, 'GAME_SFO_KEY', 'SFO key is invalid.');
    const nameBytes = bytes.subarray(key, end);
    invariant(nameBytes.every(byte => byte >= 32 && byte <= 126), 'GAME_SFO_KEY', 'SFO key is not ASCII.');
    const name = nameBytes.toString('ascii');
    invariant(!Object.hasOwn(result, name), 'GAME_SFO_DUPLICATE', 'SFO contains duplicate fields.');
    if (capacity) spans.push([start, start + capacity]);
    result[name] = { format, bytes: bytes.subarray(start, start + size) };
  }
  spans.sort((a, b) => a[0] - b[0]);
  invariant(spans.every((span, i) => !i || spans[i - 1][1] <= span[0]), 'GAME_SFO_OVERLAP', 'SFO fields overlap.');
  return result;
}
function sfoText(fields, name) {
  const field = fields[name];
  invariant([0x004, 0x204].includes(field?.format) && field.bytes.length > 0 && field.bytes.at(-1) === 0, 'GAME_SFO_FIELD', `SFO ${name} is missing or not a text field.`);
  return string(field.bytes);
}
export function inspectGameMetadata(format, data, expectedTitle) {
  const bytes = Buffer.from(data);
  invariant(GAME_TITLE.test(expectedTitle) && bytes.length >= 20 && bytes.length <= 1024 * 1024,
    'GAME_METADATA_SIZE', 'Invalid game metadata identity or size.');
  const platform = /^(PPSA|PPSB|MOUU)/.test(expectedTitle) ? 'ps5' : 'ps4';
  let titleId, version, name;
  if (format === 'json' && platform === 'ps5') {
    let json; const text = string(bytes);
    try { json = JSON.parse(text); } catch { throw new PmError('GAME_JSON', 'Mounted game metadata is not valid JSON.'); }
    invariant(json && !Array.isArray(json) && typeof json === 'object', 'GAME_JSON', 'Game metadata is not an object.');
    titleId = json.titleId; version = json.contentVersion;
    const localized = json.localizedParameters, language = localized?.defaultLanguage;
    name = typeof language === 'string' ? localized?.[language]?.titleName : localized?.['en-US']?.titleName;
  } else {
    invariant(format === 'sfo' && platform === 'ps4', 'GAME_METADATA_FORMAT', 'Metadata format differs from the running application.');
    const fields = readSfo(bytes); titleId = sfoText(fields, 'TITLE_ID'); version = sfoText(fields, 'APP_VER');
    // VERSION is the base/master version, not the active patch's APP_VER.
    name = fields.TITLE ? sfoText(fields, 'TITLE') : titleId;
  }
  invariant(titleId === expectedTitle, 'GAME_METADATA_IDENTITY', 'Mounted metadata belongs to a different title.');
  invariant(typeof version === 'string' && (platform === 'ps5' ? /^\d{2}\.\d{3}\.\d{3}$/ : /^\d{2}\.\d{2}$/).test(version),
    'GAME_VERSION', 'The mounted game version could not be verified.');
  if (typeof name !== 'string' || !name.trim() || /[\x00-\x1f\x7f]/.test(name)) name = titleId;
  return { titleId, version, platform, name: name.slice(0, 200), metadataHash: createHash('sha256').update(bytes).digest('hex') };
}
