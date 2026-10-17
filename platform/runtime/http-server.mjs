// SPDX-License-Identifier: GPL-3.0-or-later
import { Readable, Writable } from 'readable-stream';
const reasons = {200:'OK',202:'Accepted',204:'No Content',206:'Partial Content',400:'Bad Request',401:'Unauthorized',403:'Forbidden',404:'Not Found',405:'Method Not Allowed',409:'Conflict',413:'Content Too Large',416:'Range Not Satisfiable',429:'Too Many Requests',500:'Internal Server Error'};
// Host tests exercise this production parser against real TCP sockets.
// Production supplies the console's bounded native TCP adapter.
export function createHttpServer(tcpServer, options, handler) {
  if (typeof options === 'function') { handler = options; options = {}; }
  const limit = options?.maxRequestBodyBytes ?? 2 * 1048576;
  if (!Number.isSafeInteger(limit) || limit < 0 || typeof handler !== 'function') throw new TypeError('Invalid HTTP server options');
  return tcpServer(socket => {
    let header = Buffer.alloc(0), req = null, res = null, remaining = 0, received = false, rejected = false;
    socket.setTimeout(30000, () => socket.destroy());
    function reject(status) {
      if (rejected) return; rejected = true;
      socket.end(`HTTP/1.1 ${status} ${reasons[status]}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`, () => socket.destroy());
    }
    function body(chunk) {
      if (rejected || received) return;
      // Exactly one request per connection; never parse pipelined trailing data.
      const take = Math.min(remaining, chunk.length);
      if (take) { remaining -= take; if (!req.push(chunk.subarray(0,take))) socket.pause(); }
      if (!remaining) { received = true; req.complete = true; req.push(null); socket.pause(); }
    }
    function start(lines, rest) {
      const first = /^(GET|POST|PUT|DELETE|HEAD|OPTIONS) ([^\s]+) HTTP\/1\.[01]$/.exec(lines.shift());
      if (!first) return reject(400);
      const headers = Object.create(null);
      for (const line of lines) {
        const m = /^([!#$%&'*+.^_`|~0-9A-Za-z-]+):[\t ]*([^\r\n]*)$/.exec(line);
        if (!m || /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(m[2])) return reject(400);
        const name = m[1].toLowerCase(); if (name in headers) return reject(400); headers[name] = m[2].trim();
      }
      if (headers['transfer-encoding'] || headers.expect && headers.expect.toLowerCase() !== '100-continue' || headers['content-length'] !== undefined && !/^(0|[1-9]\d*)$/.test(headers['content-length'])) return reject(400);
      remaining = Number(headers['content-length'] || 0);
      if (!Number.isSafeInteger(remaining) || remaining > limit) return reject(413);
      // Large native HTTP clients may wait for this before sending any body.
      // Validate framing and limits first so rejected requests never get a go-ahead.
      if (headers.expect) socket.write('HTTP/1.1 100 Continue\r\n\r\n');
      req = new Readable({ highWaterMark: 65536, read() { if (!received && !rejected) socket.resume(); } });
      Object.assign(req, { url: first[2], method: first[1], headers, socket, complete: false, aborted: false });
      req.on('error', () => {});
      let started = false;
      res = new Writable({
        write(data,_encoding,cb) { head(); socket.write(data,cb); },
        // The native Duplex cannot observe peer EOF while body reads are
        // paused. Release this one-request connection after every queued write
        // flushes; otherwise completed renderer polls exhaust its 24 slots.
        final(cb) { head(); socket.end(() => { socket.destroy(); cb(); }); },
        destroy(error,cb) { if (error) socket.destroy(); cb(error); }
      });
      res.statusCode = 200; res.headers = {};
      res.setHeader = (key,value) => { res.headers[key] = value; };
      res.writeHead = (status,extra = {}) => { res.statusCode = status; Object.assign(res.headers,extra); head(); };
      function head() {
        if (started) return; started = true; res.headersSent = true;
        const values = { Connection: 'close', 'X-Content-Type-Options': 'nosniff', ...res.headers };
        socket.write(`HTTP/1.1 ${res.statusCode} ${reasons[res.statusCode] || 'Response'}\r\n` +
          Object.entries(values).map(([k,v]) => `${k}: ${String(v).replace(/[\r\n]/g,'')}`).join('\r\n') + '\r\n\r\n');
      }
      res.on('error', () => {});
      Promise.resolve().then(() => handler(req,res)).catch(() => {
        if (!started && !socket.destroyed) { res.writeHead(500); res.end('Request failed'); } else socket.destroy();
      });
      body(rest);
    }
    socket.on('data', chunk => {
      if (rejected || received) return;
      if (req) return body(chunk);
      header = Buffer.concat([header,chunk]);
      const split = header.indexOf('\r\n\r\n');
      if (split < 0) { if (header.length > 16384) reject(400); return; }
      if (split > 16384) return reject(400);
      const data = header; header = null; start(data.subarray(0,split).toString('latin1').split('\r\n'), data.subarray(split+4));
    });
    function aborted() {
      if (req && !received && !req.destroyed) { req.aborted = true; req.destroy(Object.assign(new Error('HTTP upload ended before Content-Length'), {code:'UPLOAD_TRUNCATED'})); }
    }
    socket.on('end', aborted);
    socket.on('error', aborted);
    socket.on('close', () => { aborted(); res?.emit('close'); });
  });
}
