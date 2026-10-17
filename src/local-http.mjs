// SPDX-License-Identifier: GPL-3.0-or-later
import { createConnection } from 'node:net';
import { PmError, invariant } from './errors.mjs';
// Private loopback client. HTTP/1.0 asks upstream to terminate the response;
// close-framed and Content-Length bodies are supported. No redirects or proxies.
export function localRequest(port, route, { timeout = 2000, maxBytes = 256 * 1024, connect = createConnection, method = 'GET', body = null, acceptedStatuses = [200] } = {}) {
  invariant(Number.isInteger(port) && port > 0 && port < 65536 && /^\/[A-Za-z0-9_/.:-]*$/.test(route), 'LOCAL_REQUEST', 'Invalid local service endpoint.');
  invariant(method === 'GET' && body === null || method === 'POST' && body && typeof body === 'object' && !Array.isArray(body),
    'LOCAL_REQUEST', 'Local requests must be GET or a JSON-object POST.');
  const payload = body === null ? Buffer.alloc(0) : Buffer.from(JSON.stringify(body));
  invariant(Array.isArray(acceptedStatuses) && acceptedStatuses.length > 0 && acceptedStatuses.every(code => Number.isInteger(code) && code >= 200 && code <= 599), 'LOCAL_REQUEST', 'Invalid expected HTTP statuses.');
  invariant(payload.length <= 4096, 'LOCAL_REQUEST', 'The local service request exceeds its size limit.');
  return new Promise((resolve, reject) => {
    const socket = connect({ host: '127.0.0.1', port }); let bytes = 0, parts = [], finished = false;
    const fail = error => { if (finished) return; finished = true; clearTimeout(timer); socket.destroy(); reject(error); };
    const timer = setTimeout(() => fail(new PmError('LOCAL_TIMEOUT', 'The console service did not respond.')), timeout);
    socket.once('connect', () => {
      const headers = `${method} ${route} HTTP/1.0\r\nHost: 127.0.0.1:${port}\r\nConnection: close\r\n` +
        (method === 'POST' ? `Content-Type: application/json\r\nContent-Length: ${payload.length}\r\n` : '') + '\r\n';
      socket.write(Buffer.concat([Buffer.from(headers), payload]));
    });
    socket.on('error', error => fail(error));
    socket.on('data', data => {
      bytes += data.length;
      if (bytes > maxBytes + 8192) return fail(new PmError('LOCAL_RESPONSE_LIMIT', 'The service response exceeded its bound.'));
      parts.push(data);
    });
    socket.once('end', () => {
      if (finished) return;
      try {
        const data = Buffer.concat(parts), boundary = data.indexOf('\r\n\r\n');
        invariant(boundary >= 0 && boundary <= 8192, 'LOCAL_HTTP', 'Invalid service HTTP headers.');
        const lines = data.subarray(0, boundary).toString('ascii').split('\r\n');
        const status = /^HTTP\/1\.[01] (\d{3})(?: |$)/.exec(lines.shift());
        invariant(status && acceptedStatuses.includes(Number(status[1])), 'LOCAL_HTTP_STATUS', 'The service did not return an expected response.');
        const headers = {};
        for (const line of lines) {
          const split = line.indexOf(':'); invariant(split > 0, 'LOCAL_HTTP', 'Malformed service header.');
          const key = line.slice(0, split).toLowerCase(), value = line.slice(split + 1).trim();
          invariant(!Object.hasOwn(headers, key), 'LOCAL_HTTP', 'Duplicate service header.'); headers[key] = value;
        }
        invariant(!headers['transfer-encoding'] && (!headers['content-encoding'] || headers['content-encoding'] === 'identity'),
          'LOCAL_HTTP_ENCODING', 'The service returned an unsupported response encoding.');
        const body = data.subarray(boundary + 4);
        invariant(body.length <= maxBytes && (!headers['content-length'] || /^\d+$/.test(headers['content-length']) && Number(headers['content-length']) === body.length),
          'LOCAL_HTTP_LENGTH', 'The service response is incomplete or too large.');
        finished = true; clearTimeout(timer); socket.destroy(); resolve({ status: Number(status[1]), headers, text: body.toString('utf8') });
      } catch (error) { fail(error); }
    });
    socket.once('close', () => { if (!finished) fail(new PmError('LOCAL_CONNECTION_CLOSED', 'The service closed before its response completed.')); });
  });
}
export const localGet = (port, route, options) => localRequest(port, route, { ...options, method: 'GET', body: null });
