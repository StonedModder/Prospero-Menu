// SPDX-License-Identifier: GPL-3.0-or-later
// Streamable HTTP JSON response mode, MCP 2025-06-18 / 2025-03-26.
import { timingSafeEqual } from 'node:crypto';
import { invariant, publicError } from './errors.mjs';
const versions = ['2025-06-18', '2025-03-26'];
const object = properties => ({ type: 'object', properties, additionalProperties: false });
const string = { type: 'string' };
const tools = [
  { name: 'console_status', description: 'Current ProsperoMenu readiness and verified running-game identity.', inputSchema: object({}), read: true },
  { name: 'cheat_catalog', description: 'List imported exact-version cheat packs.', inputSchema: object({}), read: true },
  { name: 'cheat_inspect', description: 'Read a pack and last verified operations; does not apply patches.', inputSchema: { ...object({ sha256: string }), required: ['sha256'] }, read: true },
  { name: 'cheat_set', description: 'Queue a verified cheat transaction. Requires the separate memory-write switch on the phone.',
    inputSchema: { ...object({ sha256: string, cheatId: string, enabled: { type: 'boolean' }, requestKey: string }), required: ['sha256', 'cheatId', 'enabled', 'requestKey'] }, read: false },
  { name: 'memory_inspect', description: 'Queue module information, a read up to 64 KiB, or a hex/wildcard scan up to 16 MiB. Each chunk briefly stops/resumes the exact game. Offsets are module-relative; scan chunks are not one atomic snapshot.',
    inputSchema: { ...object({ mode: { enum: ['module', 'read', 'scan'] }, module: string, offset: string,
      length: { type: 'integer', minimum: 0, maximum: 16777216 }, pattern: string, alignment: { enum: [1, 2, 4, 8] }, requestKey: string }), required: ['mode', 'module', 'requestKey'] }, read: false },
  { name: 'memory_result', description: 'Get a completed inspection result from session RAM (five-minute expiry).', inputSchema: { ...object({ id: string }), required: ['id'] }, read: true },
  { name: 'memory_refine', description: 'Narrow retained scan candidates using changed/unchanged bytes, a same-width hex/wildcard pattern, or an explicit little-endian numeric comparison. Requires an unexpired result from the same game and module. Does not search outside retained candidates or write memory.',
    inputSchema: { ...object({ source: string, comparison: { enum: ['changed', 'unchanged', 'equal', 'different', 'increased', 'decreased'] }, pattern: string,
      dataType: { enum: ['u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'u64', 'i64', 'f32', 'f64'] }, requestKey: string }), required: ['source', 'comparison', 'requestKey'] }, read: false },
  { name: 'operation_status', description: 'Read an operation status without exposing private job arguments.', inputSchema: { ...object({ id: string }), required: ['id'] }, read: true },
  { name: 'operation_cancel', description: 'Request cancellation of a memory scan between safely completed reads.', inputSchema: { ...object({ id: string }), required: ['id'] }, read: false },
  { name: 'operation_reconcile', description: 'Check the original memory or cheat receipt. Never automatically replays native work.', inputSchema: { ...object({ id: string }), required: ['id'] }, read: false }
];
export class DeveloperMcp {
  constructor(runtime) { this.runtime = runtime;this.secret = null;this.grant = null;this.expires = 0;this.sessions = new Map(); }
  enabled() { return this.runtime.store.snapshot().settings.developer.enabled && !this.runtime.graph.safe; }
  revoke() { this.secret = null;this.grant = null;this.expires = 0;this.sessions.clear();this.runtime.memory?.forget(); }
  issue() {
    invariant(this.enabled(), 'DEVELOPER_DISABLED', 'Enable developer tools before connecting an MCP client.');
    this.revoke();this.secret = this.runtime.id() + this.runtime.id();this.grant = this.runtime.id();this.expires = this.runtime.now() + 3600000;
    return { token: this.secret, expires: this.expires, path: '/mcp', transport: 'streamable-http', protocolVersions: versions };
  }
  authenticate(header) {
    if (!this.enabled() || this.expires <= this.runtime.now()) this.revoke();
    const offered = /^Bearer ([a-f0-9]{64})$/.exec(header || '')?.[1];
    invariant(this.secret && offered && timingSafeEqual(Buffer.from(offered), Buffer.from(this.secret)),
      'MCP_AUTH', 'Enable developer tools and create an MCP access token on the ProsperoMenu page.', { status: 401 });
  }
  async call(name, args) {
    invariant(args && typeof args === 'object' && !Array.isArray(args), 'MCP_ARGUMENTS', 'Tool arguments must be an object.');
    const definition = tools.find(tool => tool.name === name);
    invariant(definition, 'MCP_TOOL', 'Unknown MCP tool.');
    invariant(Object.keys(args).every(k => Object.hasOwn(definition.inputSchema.properties, k)) &&
      (definition.inputSchema.required || []).every(k => Object.hasOwn(args, k)), 'MCP_ARGUMENTS', 'Unexpected or missing tool arguments.');
    const r = this.runtime;
    switch (name) {
      case 'console_status': return r.snapshot();
      case 'cheat_catalog': return r.cheats?.list() || [];
      case 'cheat_inspect': return r.execute('cheat.inspect', args);
      case 'cheat_set': {
        invariant(r.store.snapshot().settings.developer.memoryWrites, 'MEMORY_WRITES_DISABLED', 'Turn on MCP memory writes from the phone before applying cheats.');
        const { requestKey, ...input } = args;
        const job = await r.jobs.enqueue('cheat.set', { ...input, developerSession: r.session, developerGrant: this.grant }, requestKey);
        return { id: job.id, status: job.status };
      }
      case 'memory_inspect': {
        const { requestKey, ...input } = args;const job = await r.jobs.enqueue('memory.inspect', { ...input, developerSession: r.session, developerGrant: this.grant }, requestKey);
        return { id: job.id, status: job.status };
      }
      case 'memory_refine': {
        const { requestKey, ...input } = args;const job = await r.jobs.enqueue('memory.inspect', { ...input, mode: 'refine', developerSession: r.session, developerGrant: this.grant }, requestKey);
        return { id: job.id, status: job.status };
      }
      case 'memory_result': return r.memory.result(args.id);
      case 'operation_cancel': case 'operation_reconcile': {
        const job = r.jobs.snapshot().find(j => j.id === args.id);
        invariant(job && ['memory.inspect', 'cheat.set'].includes(job.kind), 'MCP_OPERATION', 'Choose a memory or cheat operation.');
        if (name === 'operation_cancel') await r.jobs.cancel(args.id);else await r.jobs.reconcile(args.id);
        return r.snapshot().jobs.find(j => j.id === args.id);
      }
      case 'operation_status': {
        const job = await r.jobs.get(args.id);invariant(job, 'JOB_MISSING', 'Operation not found.');
        const { key, signature, requestSignature, args: input, checkpoint, ...publicJob } = job;return publicJob;
      }
    }
  }
  async message(message, sessionId, protocol) {
    invariant(message?.jsonrpc === '2.0' && typeof message.method === 'string' &&
      (message.id === undefined || typeof message.id === 'string' || Number.isSafeInteger(message.id)), 'MCP_REQUEST', 'Invalid JSON-RPC message.', { status: 400 });
    const reply = result => ({ body: { jsonrpc: '2.0', id: message.id, result }, status: 200 });
    if (message.method === 'initialize') {
      invariant(message.id !== undefined && !sessionId && typeof message.params?.protocolVersion === 'string' &&
        message.params?.clientInfo && typeof message.params.clientInfo.name === 'string', 'MCP_INITIALIZE', 'Initialize a new MCP session.', { status: 400 });
      invariant(this.sessions.size < 8, 'MCP_SESSIONS', 'Eight MCP sessions are already connected.', { status: 429 });
      const version = versions.includes(message.params.protocolVersion) ? message.params.protocolVersion : versions[0], id = this.runtime.id();
      this.sessions.set(id, { version, initialized: false, busy: false });
      return { ...reply({ protocolVersion: version, capabilities: { tools: {} }, serverInfo: { name: 'ProsperoMenu', version: '0.1.0-dev' },
        instructions: 'Native features are development builds pending console testing. Use operation IDs to observe results; accepted is not completed. Memory changes require a separate local switch.' }), session: id };
    }
    const session = this.sessions.get(sessionId);
    invariant(session, 'MCP_SESSION', 'MCP session ended. Initialize again.', { status: sessionId ? 404 : 400 });
    invariant(!protocol || protocol === session.version, 'MCP_PROTOCOL', 'MCP protocol version differs from this session.', { status: 400 });
    if (message.id === undefined) {
      if (message.method === 'notifications/initialized') session.initialized = true;
      return { status: 202 };
    }
    if (message.method === 'ping') return reply({});
    invariant(session.initialized, 'MCP_NOT_INITIALIZED', 'Finish MCP initialization first.', { status: 400 });
    if (message.method === 'tools/list') return reply({ tools: tools.map(({ read, ...tool }) => ({ ...tool,
      annotations: { readOnlyHint: read, destructiveHint: !read, openWorldHint: false } })) });
    if (message.method !== 'tools/call') return { status: 200, body: { jsonrpc: '2.0', id: message.id, error: { code: -32601, message: 'Method not found' } } };
    invariant(!session.busy, 'MCP_BUSY', 'A tool request is already in progress for this session.', { status: 429 });
    session.busy = true;
    try {
      const result = await this.call(message.params?.name, message.params?.arguments || {});
      // Disabling or rotating access during an operation prevents result delivery.
      invariant(this.enabled() && this.expires > this.runtime.now() && this.sessions.get(sessionId) === session, 'MCP_SESSION', 'MCP access ended.');
      return reply({ content: [{ type: 'text', text: JSON.stringify(result) }] });
    } catch (e) { return reply({ content: [{ type: 'text', text: JSON.stringify(publicError(e)) }], isError: true }); }
    finally { session.busy = false; }
  }
}
export function installMcp(runtime) {
  runtime.mcp = new DeveloperMcp(runtime);
  if (runtime.jobs.handlers.has('memory.inspect')) runtime.capability('mcp', true, 'Opt-in local MCP with bounded game reads and separately gated cheat writes. Console validation pending.');
  return runtime.mcp;
}
