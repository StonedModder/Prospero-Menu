// SPDX-License-Identifier: GPL-3.0-or-later
export class PmError extends Error {
  constructor(code, message, { action = null, detail = null, status = 409 } = {}) {
    super(message); this.name = 'PmError'; this.code = code;
    this.action = action; this.detail = detail; this.status = status;
  }
  toJSON() { return { code: this.code, message: this.message, action: this.action }; }
}
export function invariant(test, code, message, options) {
  if (!test) throw new PmError(code, message, options);
}
export function publicError(error) {
  return error instanceof PmError ? error.toJSON() : {
    code: 'OPERATION_FAILED', message: 'The operation failed. Its outcome needs checking.',
    action: 'Open the operation details before retrying.'
  };
}
export const clone = value => JSON.parse(JSON.stringify(value));
export function canonical(value) {
  if (Array.isArray(value)) return '[' + value.map(canonical).join(',') + ']';
  if (value && typeof value === 'object') return '{' + Object.keys(value).sort()
    .map(key => JSON.stringify(key) + ':' + canonical(value[key])).join(',') + '}';
  return JSON.stringify(value);
}
