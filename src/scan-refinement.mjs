// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
const numeric = {
  u8: [1, 'getUint8'], i8: [1, 'getInt8'], u16: [2, 'getUint16'], i16: [2, 'getInt16'],
  u32: [4, 'getUint32'], i32: [4, 'getInt32'], u64: [8, 'getBigUint64'], i64: [8, 'getBigInt64'],
  f32: [4, 'getFloat32'], f64: [8, 'getFloat64']
};
export function validateComparison(comparison, dataType, width) {
  invariant(['changed', 'unchanged', 'equal', 'different', 'increased', 'decreased'].includes(comparison),
    'SCAN_COMPARISON', 'Choose changed, unchanged, equal, different, increased or decreased.');
  if (comparison === 'increased' || comparison === 'decreased') invariant(Object.hasOwn(numeric, dataType) && numeric[dataType][0] === width,
    'SCAN_VALUE_TYPE', 'Numeric refinement needs a matching byte width and an explicit little-endian numeric type.');
  else invariant(dataType == null, 'SCAN_VALUE_TYPE', 'A numeric type is only used for increased/decreased comparisons.');
}
export function compareCandidate(before, after, comparison, pattern, dataType) {
  if (comparison === 'changed' || comparison === 'unchanged') return before.equals(after) === (comparison === 'unchanged');
  if (comparison === 'equal' || comparison === 'different') return pattern.every((value, i) => value === null || after[i] === value) === (comparison === 'equal');
  const [, getter] = numeric[dataType];
  const a = new DataView(before.buffer, before.byteOffset, before.byteLength)[getter](0, true);
  const b = new DataView(after.buffer, after.byteOffset, after.byteLength)[getter](0, true);
  if (dataType.startsWith('f') && (!Number.isFinite(a) || !Number.isFinite(b))) return false;
  return comparison === 'increased' ? b > a : b < a;
}
export function* candidateWindows(matches, width) {
  let first = 0;
  while (first < matches.length) {
    const offset = BigInt(matches[first]);let last = first + 1, end = offset + BigInt(width);
    while (last < matches.length && BigInt(matches[last]) + BigInt(width) - offset <= 65536n) { end = BigInt(matches[last]) + BigInt(width);last++; }
    yield { first, last, offset, length: Number(end - offset) };first = last;
  }
}
