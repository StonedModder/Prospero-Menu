// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
// Deliberately independent of the runtime's small server-URL shim. Download
// redirects need RFC3986 relative paths, query-only and network-path references.
export function downloadUrl(value, base = null) {
  invariant(typeof value === 'string' && value.length > 0 && value.length <= 8192 &&
    !/[\x00-\x20\x7f-\uffff\\]/.test(value), 'DOWNLOAD_URL', 'Use an HTTP or HTTPS direct-download link without spaces.');
  value = value.split('#')[0];
  if (!/^https?:\/\//i.test(value)) {
    invariant(base && !/^[a-z][a-z0-9+.-]*:/i.test(value), 'DOWNLOAD_URL', 'Only HTTP and HTTPS download links are supported.');
    const parent = downloadUrl(base), origin = /^(https?:\/\/[^/]+)(\/[^?]*)(\?.*)?$/.exec(parent);
    value = value.startsWith('//') ? parent.slice(0, parent.indexOf(':') + 1) + value :
      origin[1] + (value.startsWith('/') ? value : value.startsWith('?') ? origin[2] + value :
        !value ? origin[2] + (origin[3] || '') : origin[2].slice(0, origin[2].lastIndexOf('/') + 1) + value);
  }
  const match = /^(https?):\/\/([^/?#]+)([^?#]*)(\?[^#]*)?$/i.exec(value);
  invariant(match, 'DOWNLOAD_URL', 'The download link is incomplete.');
  const [, protocol, authority, pathname, query = ''] = match;
  // No credentials, percent-encoded host tricks or platform-specific URL forms.
  const host = /^(\[[a-f0-9:.]+\]|[a-z0-9.-]+)(?::([0-9]{1,5}))?$/i.exec(authority);
  invariant(host && (!host[2] || Number(host[2]) >= 1 && Number(host[2]) <= 65535) &&
    host[1].length <= 255 && !host[1].includes('..'), 'DOWNLOAD_URL', 'Use a link with a valid server name and port, without a username or password.');
  const parts = [];
  for (const piece of (pathname || '/').split('/').slice(1)) {
    if (piece === '..') parts.pop();else if (piece !== '.') parts.push(piece);
  }
  if (/\/(?:\.|\.\.)$/.test(pathname)) parts.push('');
  return protocol.toLowerCase() + '://' + authority.toLowerCase() + '/' + parts.join('/') + query;
}
export function redirectedDownloadUrl(location, from) {
  const next = downloadUrl(location, from);
  invariant(!from.startsWith('https:') || next.startsWith('https:'), 'DOWNLOAD_DOWNGRADE', 'This HTTPS link redirected to an insecure HTTP download. Use its HTTPS direct link.');
  return next;
}
