// SPDX-License-Identifier: GPL-3.0-or-later
import { clone, invariant, PmError } from './errors.mjs';
import { mergeSettings } from './settings.mjs';
import { requireFirmware } from './firmware.mjs';
import { ServiceGraph } from './services.mjs';
import { JobQueue } from './jobs.mjs';
import { SessionHistory } from './history.mjs';
import { AdaptiveNotices } from './notices.mjs';
import { SupportRecorder } from './diagnostics.mjs';

export const FEATURES = Object.freeze([
  ['deck', 'Command deck'], ['game-overlay', 'In-game command deck'], ['phone', 'Phone control'],
  ['payloads', 'Payload loader'], ['ftp', 'FTP server'], ['inbox', 'Payload inbox'],
  ['packages', 'Background package installation'], ['shadowmount', 'ShadowMount library'],
  ['kstuff', 'kstuff readiness'], ['payload-manager', 'Payload Manager'],
  ['fps', 'Game FPS'], ['resources', 'Resource overlay'], ['ip-overlay', 'IP overlay'],
  ['cheats', 'Game cheats'], ['saves', 'Save extraction and export'], ['mcp', 'Developer MCP'],
  ['profiles', 'Game loadouts'], ['updates', 'Staged updates and rollback']
]);
export class PmRuntime {
  constructor(store, platform, { id, now = Date.now } = {}) {
    this.store = store; this.platform = platform; this.id = id; this.now = now;
    this.sequence = 0; this.session = id(); this.game = null; this.started = false;
    this.inboxItems = [];
    const changed = () => { this.sequence++;this.notices?.observe();this.diagnostics?.observe(); };
    this.graph = new ServiceGraph({ changed, allowOptional: id => this.recovery?.permits(id) === true });
    this.jobs = new JobQueue(store, { id, now, session: this.session, changed, report: event => platform.operationLog?.(event),
      evidence: (job, value) => this.diagnostics?.receipt(job, value), admit: (kind, args) => {
        if (kind === 'package.verify' || kind === 'update.stage' || kind === 'file.import') return;
        const permitted = this.recovery?.permitsJob(kind, args);
        invariant(!this.recovery?.pending || permitted, 'STARTUP_BUSY', 'Native startup is in progress. Wait for it to finish.');
        invariant(!this.graph.safe || permitted, kind === 'component.start' ? 'SAFE_START' : 'SAFE_STARTUP',
          'Native operations are held during recovery. Review startup first.');
      } });
    this.changes = new SessionHistory(this);
    this.notices = new AdaptiveNotices(this);
    this.diagnostics = new SupportRecorder(this);
    this.capabilities = new Map(FEATURES.map(([id, name]) => [id,
      { id, name, available: false, reason: 'Native integration is not installed in this build.' }]));
  }
  capability(id, available, reason = null) {
    invariant(this.capabilities.has(id), 'FEATURE_UNKNOWN', 'Unknown capability.');
    Object.assign(this.capabilities.get(id), { available, reason }); this.sequence++;
  }
  async start() {
    invariant(!this.started, 'ALREADY_STARTED', 'ProsperoMenu is already started.');
    await this.diagnostics.open();
    try {
      this.identity = await this.platform.identity();
      this.diagnostics.identity();
      if (this.identity.mode === 'console') requireFirmware(this.identity.firmware);
      const prior = this.store.snapshot();
      // A payload that was accepted and is running is the whole precondition for
      // starting. A persisted recovery flag must never hold native startup or
      // disable features: it latched permanently and left the menu dead on every
      // boot, with nothing in the log to say why.
      this.graph.safe = false;
      this.diagnostics.record('startup', { status: 'state-open', evidence: { safe: this.graph.safe } });
      await this.store.transaction(state => {
        state.startup = { pending: true, session: this.session, safe: this.graph.safe, native: prior.startup.native || null };
        if (this.graph.safe) state.settings = clone(state.knownGood);
        // Recovery must preserve the user's latest recording choice.
        state.settings.developer.diagnostics = prior.settings.developer.diagnostics;
        // Authorization to write memory is deliberately never carried across boots.
        state.settings.developer.memoryWrites = false;
      });
      await this.jobs.recover();
      this.diagnostics.observe();
      this.diagnostics.record('startup', { status: 'installing-adapters' }); await this.diagnostics.flush();
      await this.platform.install(this);
      await this.packageRecovery?.();
      this.graph.validate(); this.started = true;
      // 'ready' here covers this coordinator only, never unstarted native services.
      await this.store.transaction(state => { state.startup.pending = false; });
      // A served webpage does not prove the native console renderer is running.
      this.capability('phone', true);
      this.notices.observe();
      this.diagnostics.record('startup', { status: 'coordinator-ready' }); await this.diagnostics.flush();
      return this;
    } catch (error) {
      this.diagnostics.record('startup', { status: 'failed', code: error.code });
      this.diagnostics.finish('startup-failed'); await this.diagnostics.flush(); throw error;
    }
  }
  overlaySettings(overlay = this.store.snapshot().settings.overlay) { return { ...overlay, ...this.loadouts?.overlay() }; }
  snapshot({ internal = false } = {}) {
    const state = this.store.snapshot(), services = this.graph.snapshot();
    return { name: 'ProsperoMenu', version: '0.1.0-dev', sequence: this.sequence, revision: state.revision,
      session: this.session, identity: this.identity, safe: this.graph.safe, game: this.game,
      gameStatus: this.gameStatus ? clone(this.gameStatus) : null,
      telemetry: this.telemetry ? this.telemetry.snapshot() : null,
      gameInput: this.gameInput?.observation ? clone(this.gameInput.observation) : null,
      settings: { ...state.settings, overlay: this.overlaySettings(state.settings.overlay) }, services, capabilities: [...this.capabilities.values()].map(clone),
      notices: this.notices.snapshot(state, services),
      diagnostics: this.diagnostics.snapshot(state),
      recovery: this.recovery?.snapshot(state) || null,
      updates: this.updates?.snapshot() || null,
      loadouts: this.loadouts ? this.loadouts.snapshot() : null,
      saves: this.saves ? this.saves.snapshot() : null,
      startupPayloads: this.startupPayloads?.snapshot() || null,
      bootAnimation: this.bootAnimation ? { count: this.bootAnimation.count ?? 0, playing: !!this.bootAnimation.playing } : null,
      downloads: { available: Boolean(this.downloads) },
      packageReceiver: this.packageReceiver?.snapshot() || null,
      inbox: clone(this.inboxItems), packages: this.packageInbox ? clone(this.packageItems || []) : null,
      cheats: this.cheats ? this.cheats.list() : null,
      library: this.libraryState ? { count: this.libraryState.games.length,
        updatedAt: this.libraryState.updatedAt, error: clone(this.libraryState.error), available: this.libraryState.available } : null,
      jobs: internal ? this.jobs.snapshot(state.jobs) : state.jobs.map(({ key, signature, requestSignature, args, checkpoint, ...job }) => job),
      history: this.changes.snapshot(state),
      queue: { halted: this.jobs.halted, error: this.jobs.fault,
        archived: Object.values(state.jobArchive || {}).reduce((sum, ref) => sum + ref.count, 0) } };
  }
  async settings(patch, revision) {
    invariant(Number.isSafeInteger(revision), 'REVISION_REQUIRED', 'Settings require the displayed state revision.', { status: 400 });
    await this.store.transaction(state => this.applyPreferences(state, patch), revision);
    await this.preferencesApplied(patch);return this.snapshot();
  }
  applyPreferences(state, patch, reversalOf = null) {
    const before = state.settings, requested = mergeSettings(before, patch);
    const guarded = [['fps', requested.overlay.fps], ['resources', requested.overlay.ram || requested.overlay.cpu],
      ['ip-overlay', requested.overlay.ip], ['mcp', requested.developer.enabled], ['payloads', requested.services.inboxAutoRun]];
    for (const [feature, enabled] of guarded) if (enabled) invariant(this.capabilities.get(feature).available,
      'FEATURE_UNAVAILABLE', this.capabilities.get(feature).reason);
    state.settings = requested;
    this.changes.settingsChanged(state, before, requested, reversalOf);
  }
  async preferencesApplied(patch) {
    if (!this.store.snapshot().settings.developer.enabled) this.mcp?.revoke();
    if (patch.overlay) { this.loadouts?.overrideOverlay(patch.overlay);this.telemetry?.reset(); }
    this.sequence++;
    this.diagnostics.configure(this.store.snapshot().settings.developer.diagnostics);
    await this.diagnostics.flush();
  }
  async restorePreferences(id, proof) {
    let patch = null;
    await this.store.transaction(state => {
      const entry = state.history.find(item => item.id === id);
      invariant(entry?.change === 'settings', 'HISTORY_MISSING', 'This preference change is no longer in history.');
      if (entry.undone) return;
      this.changes.configGuard(entry, proof, state);
      patch = clone(entry.before);entry.undone = { at: this.now() };
      this.applyPreferences(state, patch, id);
    });
    if (patch) await this.preferencesApplied(patch);
    return { reversed: true, id, message: 'Previous preferences restored.' };
  }
  async execute(command, args) {
    invariant(this.started, 'NOT_READY', 'ProsperoMenu startup is incomplete.');
    switch (command) {
      case 'package.receiver.set':
        invariant(this.packageReceiver, 'FEATURE_UNAVAILABLE', 'Package receiving is unavailable.');return this.packageReceiver.setEnabled(args.enabled);
      case 'payload.startup.configure':
        invariant(this.startupPayloads, 'FEATURE_UNAVAILABLE', 'Startup payload options are not installed.');return this.startupPayloads.configure(args);
      case 'autoload.scan':
        invariant(this.autoload, 'FEATURE_UNAVAILABLE', 'Autoload configuration is not installed.'); return this.autoload.scan();
      case 'autoload.save':
        invariant(this.autoload, 'FEATURE_UNAVAILABLE', 'Autoload configuration is not installed.'); return this.autoload.save(args);
      case 'dumper.status':
        invariant(this.dumper, 'FEATURE_UNAVAILABLE', 'App Dumper support is not installed.'); return this.dumper.status();
      case 'dumper.library':
        invariant(this.dumper, 'FEATURE_UNAVAILABLE', 'App Dumper support is not installed.'); return this.dumper.library();
      case 'dumper.devices':
        invariant(this.dumper, 'FEATURE_UNAVAILABLE', 'App Dumper support is not installed.'); return this.dumper.devices();
      case 'dumper.start':
        invariant(this.dumper, 'FEATURE_UNAVAILABLE', 'App Dumper support is not installed.'); return this.dumper.start(args);
      case 'dumper.abort':
        invariant(this.dumper, 'FEATURE_UNAVAILABLE', 'App Dumper support is not installed.'); return this.dumper.abort();
      case 'files.list': return this.fileManager.browse(args);
      case 'download.delete': return this.downloadLibrary.remove(args);
      case 'files.browse':
        invariant(this.localFiles, 'FEATURE_UNAVAILABLE', 'Local storage browsing is not installed.');return this.localFiles.browse(args);
      case 'settings.update': return this.settings(args.patch, args.revision);
      case 'bootanim.preview': {
        invariant(this.bootAnimation?.preview, 'FEATURE_UNAVAILABLE', 'The boot animation service is not installed.');
        const result = await this.bootAnimation.preview(args);
        return { ...result, message: result.started ? 'Playing ' + result.clip : 'Preview skipped: ' + result.reason };
      }
      case 'history.preview': return this.changes.preview(args);
      case 'history.undo': return this.changes.undo(args);
      case 'notice.dismiss': return this.notices.dismiss(args);
      case 'diagnostics.export': return this.diagnostics.export();
      case 'diagnostics.clear': return this.diagnostics.clear();
      case 'startup.preview':
      case 'startup.resume':
        invariant(this.recovery, 'FEATURE_UNAVAILABLE', 'Native startup recovery is not installed.');
        return command === 'startup.preview' ? this.recovery.preview() : this.recovery.resume(args);
      case 'update.begin':
      case 'update.status':
      case 'update.refresh':
      case 'update.arm':
      case 'update.revert':
      case 'update.discard':
      case 'update.retire':
        invariant(this.updates,'FEATURE_UNAVAILABLE','Update recovery is not installed.');
        if(command==='update.begin')return this.updates.begin(args);
        if(command==='update.status')return this.updates.status(args);
        if(command==='update.refresh')return this.updates.refresh();
        if(command==='update.retire')return this.updates.retire(args);
        return this.updates.select(command.slice(7),args);
      case 'save.list':
        invariant(this.saves, 'FEATURE_UNAVAILABLE', 'Save helper is not installed.');return this.saves.list(args);
      case 'loadout.save':
      case 'loadout.remove':
      case 'loadout.preview':
      case 'loadout.overlay.release': {
        invariant(this.loadouts, 'FEATURE_UNAVAILABLE', 'Loadout service is not installed.');
        if (command === 'loadout.save') return this.loadouts.save(args);
        if (command === 'loadout.remove') return this.loadouts.remove(args);
        if (command === 'loadout.preview') return this.loadouts.preview(args);
        return this.loadouts.releaseOverlay();
      }
      case 'developer.connect':
        invariant(this.mcp, 'FEATURE_UNAVAILABLE', 'MCP is not installed.');return this.mcp.issue();
      case 'developer.disconnect': this.mcp?.revoke();return { disconnected: true };
      case 'memory.result':
        invariant(this.memory, 'FEATURE_UNAVAILABLE', 'Native memory inspection is not installed.');return this.memory.result(args.id);
      case 'cheat.inspect':
        invariant(this.cheats, 'FEATURE_UNAVAILABLE', 'Cheat catalog is not installed.'); return this.cheats.inspect(args.sha256);
      case 'library.refresh':
        invariant(this.library, 'FEATURE_UNAVAILABLE', 'The game library requires the native ShadowMount adapter.');
        return this.library.refresh();
      case 'library.list':
        invariant(this.library, 'FEATURE_UNAVAILABLE', 'The game library requires the native ShadowMount adapter.');
        return this.library.page(args);
      case 'storage.destinations':
        invariant(this.storageJobs, 'FEATURE_UNAVAILABLE', 'Storage operations require ShadowMount.');
        return this.storageJobs.destinations();
      case 'package.begin': {
        invariant(this.packageInbox, 'FEATURE_UNAVAILABLE', 'Package staging is not installed.');
        const result = await this.packageInbox.begin(args); this.packageItems = await this.packageInbox.list(); return result;
      }
      case 'package.status':
        invariant(this.packageInbox, 'FEATURE_UNAVAILABLE', 'Package staging is not installed.');
        return this.packageInbox.describe(args.id);
      case 'package.list':
        invariant(this.packageInbox, 'FEATURE_UNAVAILABLE', 'Package staging is not installed.');
        return this.packageItems = await this.packageInbox.list();
      case 'inbox.begin': {
        invariant(this.inbox, 'FEATURE_UNAVAILABLE', 'Payload inbox is not installed.');
        const result = await this.inbox.begin(args); this.inboxItems = await this.inbox.list(); return result;
      }
      case 'inbox.verify': {
        invariant(this.inbox, 'FEATURE_UNAVAILABLE', 'Payload inbox is not installed.');
        const result = await this.inbox.verify(args.id); this.inboxItems = await this.inbox.list(); return result;
      }
      case 'inbox.list': {
        invariant(this.inbox, 'FEATURE_UNAVAILABLE', 'Payload inbox is not installed.');
        return this.inboxItems = await this.inbox.list();
      }
      case 'job.enqueue': return this.jobs.enqueue(args.kind, args.input, args.key);
      case 'job.cancel': await this.jobs.cancel(args.id); break;
      case 'job.resume': await this.jobs.resume(args.id); break;
      case 'job.reconcile': await this.jobs.reconcile(args.id); break;
      case 'job.release': await this.jobs.release(args.id); break;
      case 'service.start': {
        invariant(!this.recovery?.pending || this.recovery.permits(args.id), 'STARTUP_BUSY', 'Wait for ordered native startup to finish.');
        // Return immediately so a phone request does not outlive its connection.
        // The graph and durable startup job publish the actual result separately.
        this.graph.start(args.id).catch(() => { this.sequence++; }); break;
      }
      case 'service.reconcile': await this.graph.reconcile(args.id); break;
      default: throw new PmError('COMMAND_UNKNOWN', 'Unknown command.', { status: 404 });
    }
    return this.snapshot();
  }
  async close() { this.consoleDeck?.stop(); this.packageReceiver?.close(); this.startupPayloads?.stop(); this.mcp?.revoke(); this.saves?.stop(); this.loadouts?.stop();this.telemetry?.stop(); this.ps4Fps?.stop(); this.gameInputRelay?.stop(); this.gameInput?.stop(); this.gameLifecycle?.stop(); this.nativeServices?.stopMonitoring(); this.nativeDeck?.stopMonitoring(); this.jobs.stopDispatch(); await this.store.tail; await this.diagnostics.close(); }
}
