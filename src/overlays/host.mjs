// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoMenu overlay host (docs/OVERLAYS.md).
//
// ELF overlays run as their own processes and talk to this host over
// 127.0.0.1 with one JSON object per line. The host owns their lifecycle:
// launch, handshake, watchdog, show/hide and stop. A hung, crashing or
// flooding overlay can only lose its own page; the menu stays usable.
import net from 'node:net';
import { LineReader, sanitizePage, applyPatch, pageRows } from './protocol.mjs';
import { OVERLAY_API } from './manifest.mjs';

export const OVERLAY_PORT = 8791;
const PING_MS = 1000, UNRESPONSIVE_MS = 3000, KILL_MS = 10000, CONNECT_MS = 15000, BACK_MS = 1500, QUIT_MS = 1500;
const hex = bytes => Buffer.from(bytes).toString('hex');

export class OverlayHost {
  constructor({ registry, fs, native = null, random, notify = () => {}, log = () => {}, now = () => Date.now(),
    schedule = (fn, ms) => setInterval(fn, ms), port = OVERLAY_PORT, services = {} }) {
    // services: { action: { permission, run(args, overlay) } } that overlays
    // may call with {"type":"request"}; a permission must be declared in the
    // overlay's manifest ("permissions": [...]).
    Object.assign(this, { registry, fs, native, random, notify, log, now, schedule, port, services });
    this.sessions = new Map(); this.active = null; this.visible = false; this.revision = 1; this.server = null; this.listeners = new Set();
  }
  bump() { this.revision = (this.revision + 1) >>> 0 || 1; }
  onExit(listener) { this.listeners.add(listener); }
  async start() {
    await new Promise((resolve, reject) => {
      this.server = net.createServer(socket => this.accept(socket));
      this.server.once('error', reject);
      this.server.listen(this.port, '127.0.0.1', () => { this.port = this.server.address().port || this.port; resolve(); });
    });
    this.timer = this.schedule(() => this.tick(), 250); this.timer?.unref?.();
    return this;
  }
  // Autostart overlays are launched only after ordered native startup
  // (kstuff, ShadowMount, the deck) has settled. Each overlay spawn holds the
  // shared native-operation lock; taken during startup, that lock made the
  // kstuff readiness probe report "busy" and startup gave up before the deck
  // was injected, so the menu never appeared.
  startAutostart() {
    if (this.autostarted) return; this.autostarted = true;
    for (const overlay of this.registry.overlays) if (overlay.kind === 'elf' && overlay.autostart) this.launch(overlay).catch(() => {});
  }
  close() {
    for (const session of this.sessions.values()) this.stop(session.overlay.id, { force: true });
    if (this.timer) clearInterval(this.timer); this.server?.close();
  }
  session(id) { return this.sessions.get(id) || null; }

  // ---- lifecycle -------------------------------------------------------
  async launch(overlay) {
    const existing = this.sessions.get(overlay.id);
    // A second launch (autostart racing a menu open) waits for the first.
    if (existing && ['starting', 'connected'].includes(existing.state)) { await existing.ready; return existing; }
    const token = hex(this.random(16));
    const session = { overlay, state: 'starting', token, socket: null, pid: -1, page: null, startedAt: this.now(), lastSeen: this.now(),
      pingAt: 0, message: 'Starting ' + overlay.name, unresponsive: false, backAt: 0, quitAt: 0, spawnSerial: 0 };
    let ready; session.ready = new Promise(resolve => { ready = resolve; });
    this.sessions.set(overlay.id, session); this.bump();
    try { await this.spawn(session); } finally { ready(); }
    return session;
  }
  async spawn(session) {
    const overlay = session.overlay;
    // Session file: how the overlay finds this host. Read by the SDK at
    // /data/ProsperoMenu/run/<id>.session as "<port> <token>". Written to a
    // temporary name and renamed, so an overlay never reads half a file.
    const file = `${this.registry.run}/${overlay.id}.session`;
    await this.fs.writeFile(file + '.tmp', `${this.port} ${session.token}\n`);
    await this.fs.rename(file + '.tmp', file);
    if (!this.native?.overlaySpawn) {
      // Development host (npm run dev): start the overlay yourself and it
      // connects using the session file.
      session.message = `Waiting for ${overlay.name} to connect (development host)`; return;
    }
    for (let attempt = 0; ; attempt++) {
      try { session.spawnSerial = this.native.overlaySpawn(overlay.id, `${overlay.folder}/${overlay.elf}`); break; }
      catch (error) {
        if (error.code === 'OVERLAY_BUSY' && attempt < 40) { await new Promise(r => setTimeout(r, 250)); continue; }
        this.fail(session, error.message || 'The overlay could not be started.'); return;
      }
    }
  }
  fail(session, message) {
    session.state = 'failed'; session.message = message; session.socket?.destroy(); session.socket = null; this.bump();
    this.log(`ProsperoMenu overlay ${session.overlay.id}: ${message}`);
  }
  stop(id, { force = false } = {}) {
    const session = this.sessions.get(id); if (!session) return;
    if (session.socket && !force) { this.send(session, { type: 'quit' }); session.quitAt = this.now(); return; }
    this.kill(session, 'Stopped');
  }
  kill(session, message) {
    session.socket?.destroy(); session.socket = null;
    if (session.pid > 0) try { this.native?.overlayKill?.(session.pid, session.overlay.id); } catch { /* already gone */ }
    session.state = 'exited'; session.message = message; session.page = null; this.bump();
  }

  // ---- connections -----------------------------------------------------
  accept(socket) {
    const reader = new LineReader(); let session = null;
    const hello = setTimeout(() => { if (!session) socket.destroy(); }, 5000); hello.unref?.();
    socket.on('data', chunk => {
      let messages;
      try { messages = reader.push(chunk); }
      catch (error) { if (session) this.fail(session, 'Protocol error: ' + error.message); socket.destroy(); return; }
      for (const message of messages) {
        if (!session) {
          session = this.handshake(socket, message);
          clearTimeout(hello);
          if (!session) { socket.destroy(); return; }
          continue;
        }
        try { this.receive(session, message); }
        catch (error) { this.fail(session, 'Protocol error: ' + error.message); socket.destroy(); return; }
      }
    });
    socket.on('error', () => {});
    socket.on('close', () => {
      clearTimeout(hello);
      if (session && session.socket === socket) {
        session.socket = null;
        if (session.state === 'connected') { session.state = 'exited'; session.message = session.quitAt ? 'Stopped' : `${session.overlay.name} closed`; session.page = null; this.bump(); }
        if (this.active === session.overlay.id && !session.quitAt) this.emitExit(session.overlay.id, session.message);
      }
    });
  }
  handshake(socket, message) {
    if (message.type !== 'hello' || typeof message.id !== 'string') return null;
    const session = this.sessions.get(message.id);
    if (!session || session.state !== 'starting' || message.token !== session.token) return null;
    if (message.api !== OVERLAY_API) { this.fail(session, `Overlay speaks API ${message.api}; this menu speaks API ${OVERLAY_API}.`); return null; }
    session.socket = socket; session.state = 'connected'; session.lastSeen = this.now(); session.message = '';
    if (Number.isInteger(message.pid)) session.pid = message.pid;
    this.send(session, { type: 'welcome', api: OVERLAY_API, menu: 'ProsperoMenu' });
    this.send(session, { type: this.active === session.overlay.id && this.visible ? 'show' : 'hide' });
    this.bump(); return session;
  }
  receive(session, message) {
    session.lastSeen = this.now(); if (session.unresponsive) { session.unresponsive = false; this.bump(); }
    switch (message.type) {
      case 'page': session.page = sanitizePage(message); session.backAt = 0; this.bump(); break;
      case 'patch': applyPatch(session.page, message); this.bump(); break;
      case 'notice': this.notify(`${session.overlay.name}: ${String(message.text ?? '').slice(0, 120)}`); break;
      case 'exit': session.backAt = 0; if (this.active === session.overlay.id) this.emitExit(session.overlay.id, ''); break;
      case 'log': this.log(`[${session.overlay.id}] ${String(message.text ?? '').slice(0, 400)}`); break;
      case 'pong': break;
      case 'request': this.request(session, message); break;
      default: throw new Error(`Unknown message type "${String(message.type).slice(0, 16)}".`);
    }
  }
  async request(session, message) {
    const id = typeof message.id === 'string' ? message.id.slice(0, 32) : '';
    const reply = result => this.send(session, { type: 'result', id, ...result });
    const service = Object.hasOwn(this.services, message.action) ? this.services[message.action] : null;
    if (!service) return reply({ ok: false, error: `Unknown request "${String(message.action).slice(0, 32)}".` });
    if (service.permission && !session.overlay.permissions?.includes(service.permission))
      return reply({ ok: false, error: `This overlay needs the "${service.permission}" permission in overlay.json.` });
    try { reply({ ok: true, ...(await service.run(message, session.overlay)) }); }
    catch (error) { reply({ ok: false, error: String(error.message || error).slice(0, 200) }); }
  }
  send(session, message) {
    if (!session.socket) return false;
    try { session.socket.write(JSON.stringify(message) + '\n'); return true; } catch { return false; }
  }
  emitExit(id, message) {
    if (this.active === id) this.active = null;
    for (const listener of this.listeners) listener(id, message);
    this.bump();
  }

  // ---- menu side -------------------------------------------------------
  async open(overlay) {
    // Like Tesla, one overlay page at a time: opening another one stops the
    // previous overlay unless it asked to keep running (keepAlive).
    for (const [id, other] of this.sessions) if (id !== overlay.id && !other.overlay.keepAlive && other.state === 'connected') this.stop(id);
    this.active = overlay.id;
    const session = await this.launch(overlay);
    if (session.state === 'connected') this.send(session, { type: 'show' });
    this.bump(); return session;
  }
  leave() {
    const session = this.active && this.sessions.get(this.active);
    if (session) this.send(session, { type: 'hide' });
    this.active = null; this.bump();
  }
  setVisible(visible) {
    if (this.visible === visible) return; this.visible = visible;
    const session = this.active && this.sessions.get(this.active);
    if (session) this.send(session, { type: visible ? 'show' : 'hide' });
  }
  // Circle: the overlay owns its page stack. It answers with a new page or
  // with "exit" at its root. Silence means it is stuck: go back anyway.
  back() {
    const session = this.active && this.sessions.get(this.active);
    if (!session || session.state !== 'connected' || session.unresponsive) return 'exit';
    this.send(session, { type: 'back' }); session.backAt = this.now(); return 'wait';
  }
  event(itemId, kind) {
    const session = this.active && this.sessions.get(this.active);
    const item = session?.page?.items.find(x => x.id === itemId);
    if (!session || session.state !== 'connected' || !item || !item.enabled) return false;
    if (kind === 'select') {
      if (item.kind === 'toggle') { item.on = !item.on; this.send(session, { type: 'event', kind: 'toggle', id: item.id, value: item.on }); }
      else if (item.kind === 'slider') return false;
      else this.send(session, { type: 'event', kind: 'select', id: item.id });
    } else if ((kind === 'next' || kind === 'previous') && item.kind === 'slider') {
      const next = Math.min(item.max, Math.max(item.min, item.current + (kind === 'next' ? item.step : -item.step)));
      if (next === item.current) return false;
      item.current = next; this.send(session, { type: 'event', kind: 'slide', id: item.id, value: next });
    } else return false;
    this.bump(); return true;
  }
  // Rows for the active overlay's page, including lifecycle states.
  view(overlay) {
    const session = this.sessions.get(overlay.id);
    // Progress states are plain information; only a failure is drawn as a
    // warning (the deck colours a disabled row's description).
    const status = (label, detail, problem = false) => ({ title: overlay.name, subtitle: overlay.version ? 'v' + overlay.version : '',
      rows: [problem ? { label, detail, enabled: false } : { label, detail, enabled: true, command: '$pm-none', args: {} }] });
    if (!session || session.state === 'starting') return status(session?.message || 'Starting ' + overlay.name, 'One moment while it starts.');
    if (session.state === 'failed') return status('Overlay could not start', session.message + ' Details are in overlay.log in its folder.', true);
    if (session.state === 'exited') return status(session.message || 'Overlay closed', 'Press Circle, then X on the overlay to start it again.');
    if (!session.page) return status('Loading ' + overlay.name, 'Connected; waiting for the first page.');
    const rows = pageRows(session.page);
    if (session.unresponsive) rows.unshift({ label: 'Overlay not responding', detail: 'Press Circle to return to the list. It is stopped after 10 seconds.', enabled: false });
    const adjustable = session.page.items.some(i => i.kind === 'slider');
    return { title: session.page.title || overlay.name, subtitle: session.page.subtitle || (overlay.version ? 'v' + overlay.version : ''),
      footer: session.page.footer || (adjustable ? 'X Select   Circle Back   Left/Right Adjust' : null), rows };
  }

  // ---- watchdog --------------------------------------------------------
  tick() {
    const now = this.now();
    if (this.native?.overlayPoll) {
      const spawn = this.native.overlayPoll();
      for (const session of this.sessions.values()) {
        if (session.state !== 'starting' || !session.spawnSerial || spawn.serial !== session.spawnSerial) continue;
        if (spawn.state === 'failed') this.fail(session, spawn.message);
        else if (spawn.state === 'spawned' && session.pid < 0) session.pid = spawn.pid;
      }
    }
    for (const session of this.sessions.values()) {
      if (session.state === 'starting' && this.native?.overlaySpawn && now - session.startedAt > CONNECT_MS)
        this.fail(session, 'The overlay did not connect within 15 seconds. Check overlay.log in its folder.');
      if (session.state !== 'connected') continue;
      if (session.quitAt && now - session.quitAt > QUIT_MS) { this.kill(session, 'Stopped'); continue; }
      if (now - session.pingAt >= PING_MS) { session.pingAt = now; this.send(session, { type: 'ping', at: now }); }
      const silent = now - session.lastSeen;
      if (silent > KILL_MS) { this.kill(session, `${session.overlay.name} stopped responding and was closed`); if (this.active === session.overlay.id) this.emitExit(session.overlay.id, session.message); continue; }
      if (silent > UNRESPONSIVE_MS && !session.unresponsive) { session.unresponsive = true; this.bump(); }
      if (session.backAt && now - session.backAt > BACK_MS) { session.backAt = 0; if (this.active === session.overlay.id) this.emitExit(session.overlay.id, ''); }
    }
  }
  snapshot() {
    return [...this.sessions.values()].map(s => ({ id: s.overlay.id, state: s.state, pid: s.pid, message: s.message, unresponsive: s.unresponsive }));
  }
}
