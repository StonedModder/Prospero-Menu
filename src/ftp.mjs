// SPDX-License-Identifier: GPL-3.0-or-later
import net from 'node:net';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { invariant } from './errors.mjs';

// Console-local share. Does not expose the state journal, credentials or arbitrary
// filesystem roots. Additional console/USB mounts require explicit native adapters.
export async function startFtp({ root, host, address, port = 2122, id, maxClients = 4, onUpload }) {
  await fs.mkdir(root, { recursive: true });
  root = await fs.realpath(root);
  if (onUpload) await fs.mkdir(path.join(root, 'payloads'), { recursive: true });
  const clients = new Set();
  const normalize = ip => String(ip).replace(/^::ffff:/, '');
  const server = net.createServer(control => {
    if (clients.size >= maxClients) { control.end('421 All FTP connections are in use.\r\n'); return; }
    const client = { control, passive: null, data: null }; clients.add(client);
    let cwd = '/', user = false, authenticated = false, pending = '', commands = Promise.resolve(), queued = 0, renameFrom = null;
    const reply = (code, text) => { if (!control.destroyed) control.write(`${code} ${String(text).replace(/[\r\n]/g, ' ')}\r\n`); };
    control.setTimeout(120000, () => control.destroy()); control.on('error', () => {});
    function clearPassive() {
      const passive = client.passive; client.passive = null;
      if (passive) { clearTimeout(passive.timer); passive.reject(Error('FTP data connection closed')); passive.socket?.destroy(); passive.server.close(); }
      client.data?.destroy(); client.data = null;
    }
    control.once('close', () => { clearPassive(); clients.delete(client); });
    async function resolve(input, { parent = false } = {}) {
      invariant(typeof input === 'string' && !/[\x00-\x1f\\]/.test(input), 'FTP_PATH', 'Invalid path');
      const virtual = path.posix.resolve(cwd, input || '.');
      invariant(!virtual.split('/').some(part => part.startsWith('.prosperomenu-')), 'FTP_PATH', 'Reserved file name');
      const target = path.join(root, ...virtual.split('/').filter(Boolean));
      let candidate = root;
      const segments = virtual.split('/').filter(Boolean);
      for (let i = 0; i < segments.length; i++) {
        candidate = path.join(candidate, segments[i]);
        try { invariant(!(await fs.lstat(candidate)).isSymbolicLink(), 'FTP_PATH', 'Symbolic links are not shared'); }
        catch (error) { if (parent && i === segments.length - 1 && error.code === 'ENOENT') break; throw error; }
      }
      return { virtual, target };
    }
    async function passive(extended) {
      clearPassive(); let accept, reject;
      const ready = new Promise((yes, no) => { accept = yes; reject = no; }); ready.catch(() => {});
      const listener = net.createServer(socket => {
        if (normalize(socket.remoteAddress) !== normalize(control.remoteAddress) || session.socket) { socket.destroy(); return; }
        socket.on('error', () => {}); socket.setTimeout(30000, () => socket.destroy());
        session.socket = socket; clearTimeout(session.timer); accept(socket);
      });
      const session = { server: listener, ready, reject, socket: null, timer: null }; client.passive = session;
      listener.on('error', reject);
      await new Promise((yes, no) => { listener.once('error', no); listener.listen(0, host, yes); });
      session.timer = setTimeout(() => { if (!session.socket) clearPassive(); }, 30000);
      const dataPort = listener.address().port;
      reply(extended ? 229 : 227, extended ? `Entering Extended Passive Mode (|||${dataPort}|)` :
        `Entering Passive Mode (${address.replaceAll('.', ',')},${dataPort >> 8},${dataPort & 255})`);
    }
    async function transfer(action) {
      const passive = client.passive;
      invariant(passive, 'FTP_PASSIVE', 'Use EPSV or PASV first');
      reply(150, 'Opening data connection');
      try {
        const socket = await passive.ready; client.data = socket;
        const detail = await action(socket);
        reply(226, typeof detail === 'string' ? detail.slice(0, 240) : 'Transfer complete');
      } catch { reply(426, 'Transfer interrupted; destination was not committed'); }
      finally { clearPassive(); }
    }
    const write = (socket, bytes) => new Promise((yes, no) => socket.write(bytes, error => error ? no(error) : yes()));
    const end = socket => new Promise((yes, no) => { socket.once('error', no); socket.end(yes); });
    async function dispatch(command, arg) {
      switch (command) {
        case 'USER': user = ['anonymous', 'prosperomenu', 'prosperomenu'].includes(arg.toLowerCase()); authenticated = false; reply(user ? 331 : 530, user ? 'LAN share; send PASS' : 'Use prosperomenu or anonymous'); return;
        case 'PASS': authenticated = user; reply(user ? 230 : 530, user ? 'ProsperoMenu share connected' : 'Send USER first'); return;
        case 'QUIT': reply(221, 'Disconnected'); control.end(); return;
        case 'SYST': reply(215, 'UNIX Type: L8'); return;
        case 'FEAT': control.write('211-Features\r\n UTF8\r\n EPSV\r\n PASV\r\n SIZE\r\n MDTM\r\n MLST type*;size*;modify*;\r\n211 End\r\n'); return;
        case 'OPTS': reply(arg.toUpperCase() === 'UTF8 ON' ? 200 : 501, 'UTF8 filenames are supported'); return;
        case 'NOOP': reply(200, 'OK'); return;
      }
      if (!authenticated) { reply(530, 'Log in first'); return; }
      switch (command) {
        case 'PWD': case 'XPWD': reply(257, `"${cwd.replaceAll('"', '""')}"`); break;
        case 'TYPE': reply(['I', 'A', 'L 8'].includes(arg.toUpperCase()) ? 200 : 504, 'Type selected'); break;
        case 'CWD': case 'CDUP': {
          const dest = await resolve(command === 'CDUP' ? '..' : arg); invariant((await fs.stat(dest.target)).isDirectory(), 'FTP_DIRECTORY', 'Not a directory');
          cwd = dest.virtual; reply(250, 'Directory changed'); break;
        }
        case 'EPSV': case 'PASV': await passive(command === 'EPSV'); break;
        case 'SIZE': case 'MDTM': {
          const { target } = await resolve(arg), stat = await fs.stat(target); invariant(stat.isFile(), 'FTP_FILE', 'Not a file');
          reply(213, command === 'SIZE' ? stat.size : new Date(stat.mtimeMs).toISOString().replace(/[-:T]/g, '').slice(0, 14)); break;
        }
        case 'LIST': case 'MLSD': {
          const { target } = await resolve(arg), entries = await fs.readdir(target, { withFileTypes: true });
          await transfer(async socket => {
            for (const entry of entries) {
              if (entry.isSymbolicLink() || entry.name.startsWith('.prosperomenu-') || /[\r\n]/.test(entry.name)) continue;
              const stat = await fs.stat(path.join(target, entry.name)), directory = stat.isDirectory();
              if (!directory && !stat.isFile()) continue;
              const modified = new Date(stat.mtimeMs).toISOString().replace(/[-:T]/g, '').slice(0, 14);
              const line = command === 'MLSD' ? `type=${directory ? 'dir' : 'file'};size=${stat.size};modify=${modified}; ${entry.name}\r\n` :
                `${directory ? 'd' : '-'}rw-r--r-- 1 prosperomenu prosperomenu ${stat.size} Jan 01 2000 ${entry.name}\r\n`;
              await write(socket, line);
            }
            await end(socket);
          }); break;
        }
        case 'RETR': {
          const { target } = await resolve(arg); invariant((await fs.stat(target)).isFile(), 'FTP_FILE', 'Not a file');
          await transfer(async socket => {
            const file = await fs.open(target, 'r');
            try { const chunk = Buffer.alloc(65536); for (;;) { const { bytesRead } = await file.read(chunk, 0, chunk.length, null); if (!bytesRead) break; await write(socket, chunk.subarray(0, bytesRead)); } await end(socket); }
            finally { await file.close(); }
          }); break;
        }
        case 'STOR': {
          const { target, virtual } = await resolve(arg, { parent: true });
          invariant(target !== root, 'FTP_FILE', 'Choose a file name');
          const temporary = path.join(path.dirname(target), '.prosperomenu-' + id() + '.part');
          await transfer(async socket => {
            const file = await fs.open(temporary, 'wx', 0o600); let committed = false;
            try {
              let sinceCheck = 16 * 1024 * 1024;
              for await (const chunk of socket) {
                if (sinceCheck >= 16 * 1024 * 1024) {
                  const capacity = await fs.statfs(root); invariant(Number(capacity.bsize) * Number(capacity.bavail) > 32 * 1024 * 1024, 'NO_SPACE', 'Share is out of space'); sinceCheck = 0;
                }
                await file.writeFile(chunk); sinceCheck += chunk.length;
              }
              invariant(!control.destroyed, 'FTP_DISCONNECTED', 'Control connection closed');
              await file.sync(); await file.close(); await fs.rename(temporary, target); committed = true;
              if (onUpload) {
                try { return await onUpload({ target, virtual }); }
                catch { return 'Transfer saved; inbox processing failed. Check the deck.'; }
              }
            } finally { await file.close(); if (!committed) try { await fs.unlink(temporary); } catch {} }
          }); break;
        }
        case 'MKD': { const dest = await resolve(arg, { parent: true }); await fs.mkdir(dest.target); reply(257, `"${dest.virtual.replaceAll('"', '""')}" created`); break; }
        case 'RNFR': renameFrom = (await resolve(arg)).target; invariant(renameFrom !== root, 'FTP_PATH', 'Cannot rename share root'); reply(350, 'Send RNTO'); break;
        case 'RNTO': {
          invariant(renameFrom, 'FTP_RENAME', 'Send RNFR first'); const dest = await resolve(arg, { parent: true });
          const source = renameFrom; renameFrom = null; await fs.rename(source, dest.target); reply(250, 'Renamed'); break;
        }
        default: reply(502, 'Command not implemented');
      }
    }
    control.on('data', chunk => {
      pending += chunk.toString('utf8');
      if (pending.length > 8192) { reply(500, 'Command too long'); control.destroy(); return; }
      let at;
      while ((at = pending.indexOf('\r\n')) >= 0) {
        const line = pending.slice(0, at); pending = pending.slice(at + 2);
        if (++queued > 32) { control.destroy(); return; }
        const space = line.indexOf(' '), command = (space < 0 ? line : line.slice(0, space)).toUpperCase(), arg = space < 0 ? '' : line.slice(space + 1);
        commands = commands.then(() => { if (!control.destroyed) return dispatch(command, arg); })
          .catch(() => reply(550, 'Operation unavailable or path rejected')).finally(() => queued--);
      }
    });
    reply(220, 'ProsperoMenu local share');
  });
  await new Promise((yes, no) => { server.once('error', no); server.listen(port, host, yes); });
  return { port: server.address().port, async stop() {
    for (const client of clients) client.control.destroy();
    server.closeAllConnections?.(); await new Promise(resolve => server.close(resolve));
  } };
}
