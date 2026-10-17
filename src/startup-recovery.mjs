// SPDX-License-Identifier: GPL-3.0-or-later
import { canonical, clone, invariant, publicError, PmError } from './errors.mjs';
const startupJob = j => ['component.start', 'deck.start', 'payload.startup'].includes(j.kind);
const unresolved = j => ['queued', 'held', 'running', 'cancelling', 'uncertain', 'interrupted'].includes(j.status);
const locksMenu = j => j.resources?.some(resource => ['native-payload-dispatch', 'shellui-hooks'].includes(resource));
const isolatedService = id => ['shadowmount','payload-manager'].includes(id);

export class StartupRecovery {
  constructor(runtime, native) {
    this.runtime = runtime; this.native = native; this.pending = null; this.target = null; this.attempt = null;
  }
  permits(id) { return Boolean(this.pending && this.target === id); }
  permitsJob(kind, args) { return kind === 'component.start' ? this.permits(args?.component) : kind === 'deck.start' && this.permits('native-deck'); }
  services(settings) {
    // ProsperoMenu never bundles or auto-starts third-party payloads (kstuff,
    // ShadowMount, Payload Manager): users autoload their own copies. The menu
    // adopts a running instance when one is detected. Only its own UI service
    // is started here.
    return [...(this.runtime.graph.definitions.has('native-deck') ? ['native-deck'] : [])];
  }
  snapshot(state = this.runtime.store.snapshot()) {
    const last = state.startup.native;
    return { available: true, running: Boolean(this.pending), target: this.target, safe: this.runtime.graph.safe,
      last: last ? { status: last.status, degraded: last.degraded === true, service: last.service, at: last.at, error: clone(last.error), services: last.services } : null,
      knownGood: state.knownGoodReceipt ? clone(state.knownGoodReceipt) : null };
  }
  async preview() {
    const r = this.runtime;
    invariant(!this.pending, 'STARTUP_BUSY', 'Startup is already being checked.');
    const state = r.store.snapshot(), probe = this.native.systemProbe(), blockers = [];
    if (probe.busy || [...r.jobs.running.values()].some(active=>locksMenu(active.job)) || r.graph.pending.size) blockers.push('A native operation is still running. Wait for it to settle.');
    if (r.jobs.halted || r.jobs.stopped) blockers.push('The work queue is stopped. Preserve diagnostics before restarting ProsperoMenu.');
    if (!probe.boot) blockers.push('The current console boot could not be identified.');
    for (const job of state.jobs.filter(j => unresolved(j) && j.status !== 'held' && !startupJob(j) && locksMenu(j)))
      blockers.push(`${job.kind}: review its previous ${job.status} operation in Activity first.`);
    const services = [], identities = [], settings = clone(state.settings);
    if (!blockers.length) {
      for (const service of this.services(settings)) {
        const definition = r.graph.definitions.get(service);
        let evidence;
        try { evidence = await definition.inspect(); }
        catch (error) { evidence = { ready: false, absent: false, reason: publicError(error).message }; }
        const status = evidence.ready ? 'adopt' : evidence.absent ? 'start' : 'blocked';
        identities.push(Object.fromEntries(['ready', 'absent', 'boot', 'pid', 'generation', 'version', 'request', 'stage', 'noSideEffects'].map(key => [key, evidence[key] ?? null])));
        services.push({ id: service, status, reason: status === 'blocked' ? evidence.reason || evidence.message || 'Existing state remains uncertain. Restart the console before another attempt.' : null });
        if (status === 'blocked' && !isolatedService(service)) blockers.push(`${service}: ${services.at(-1).reason}`);
      }
      // Inspection is read-only. Reconciliation and any new dispatch require confirmation.
      for (const job of state.jobs.filter(j => startupJob(j) && j.status === 'interrupted')) {
        const handler = r.jobs.handlers.get(job.kind);
        try {
          const result = await handler?.reconcile?.(clone(job));
          if (!(result?.absent || result?.completed || result?.expired)) blockers.push(`${job.kind}: its previous result remains uncertain.`);
        } catch { blockers.push(`${job.kind}: the previous receipt could not be verified.`); }
      }
    }
    const current = this.native.systemProbe();
    invariant(r.store.snapshot().revision === state.revision && (!current.busy || probe.busy) && current.boot === probe.boot,
      'RECOVERY_CHANGED', 'Startup state changed during inspection. Review recovery again.');
    const stable = { session: r.session, boot: probe.boot || null, revision: state.revision, settings, services, identities, blockers };
    return { available: blockers.length === 0, proof: r.store.checksum(canonical(stable)), revision: state.revision,
      services, blockers, message: 'Recheck existing services, reconcile prior startup receipts, then start only absent configured components.' };
  }
  async resume({ proof }) {
    if (this.pending) return { accepted: true, running: true };
    const review = await this.preview();
    invariant(review.available, 'RECOVERY_BLOCKED', review.blockers.join(' '));
    invariant(typeof proof === 'string' && proof === review.proof, 'RECOVERY_CHANGED', 'Startup state changed. Review recovery again.');
    // Set the reservation before returning to the phone. Other native jobs stay
    // held throughout recovery; only the current ordered startup step is admitted.
    this.begin(true, review.revision);return { accepted: true, running: true };
  }
  startConfigured() {
    if (this.pending) return this.pending;
    const r = this.runtime, state = r.store.snapshot(), probe = this.native.systemProbe();
    const previousBoot = state.startup.native?.boot || state.jobs.findLast(j =>
      ['component.start', 'deck.start'].includes(j.kind) && j.args?.boot)?.args.boot;
    const freshBoot = !probe.busy && Boolean(probe.boot && previousBoot && probe.boot !== previousBoot);
    // Startup is never held on a persisted flag: if this payload was accepted and
    // is running, its services and the deck start. The only thing left from the
    // old behaviour is not repeating a *file operation* that ran in this same
    // boot (freshBoot stays false then, and each step's own guard handles it).
    if (!freshBoot && r.store.recovered && r.graph.safe) {
      console.log('ProsperoMenu startup: not a fresh boot; ordered services start anyway');
    }
    return this.begin(freshBoot, undefined, freshBoot);
  }
  begin(recovery, reviewedRevision, freshBoot = false) {
    if (this.pending) return this.pending;
    this.attempt = this.runtime.id();
    this.pending = Promise.resolve().then(() => this.run(recovery, reviewedRevision, freshBoot))
      .catch(error => {
        // A rejected startup run was swallowed here, which left the menu dead
        // with no line in the log at all.
        console.log('ProsperoMenu startup run failed: ' + (error?.code || '') + ' ' + String(error?.message || error).slice(0, 200));
        throw error;
      })
      .finally(() => {
      this.pending = null;this.target = null;this.runtime.sequence++;
      if (!this.runtime.graph.safe && !this.runtime.jobs.snapshot().some(j=>unresolved(j)&&j.status!=='held'&&locksMenu(j))) this.runtime.startupPayloads?.run();
    });
    // Interactive launch has no request waiting for completion. Keep its error
    // recorded and observed rather than creating an unhandled rejection.
    this.pending.catch(() => {});return this.pending;
  }
  async run(recovery, reviewedRevision, freshBoot = false) {
    const r = this.runtime, candidate = clone(r.store.snapshot().settings), services = this.services(candidate);
    let failure = null, boot = null;const issues=[];
    const isolate=(service,error)=>{
      const issue={service,error:publicError(error)};issues.push(issue);
      console.log('ProsperoMenu optional service unavailable: ' + service + ' ' + issue.error.code + ' ' + issue.error.message);
      r.diagnostics?.record('startup',{status:'optional-unavailable',target:service,code:issue.error.code});
    };
    try {
      invariant(!r.jobs.halted && !r.jobs.stopped && ![...r.jobs.running.values()].some(active=>locksMenu(active.job)) && !r.graph.pending.size,
        'STARTUP_BUSY', 'Wait for active operations before starting components.');
      // Mount and game-input workers cannot survive a console boot. Settle
      // their old intent through each handler's checked path, never replay it.
      boot=this.native.systemProbe().boot;
      for(const job of r.jobs.snapshot().filter(j=>{
        const prior=j.kind==='library.mount'?j.args?.service?.boot:j.kind==='game.input.prepare'?j.args?.boot:null;
        return j.status==='interrupted'&&prior&&boot&&prior!==boot;
      }))
        await r.jobs.reconcile(job.id);
      // Read existing installer evidence, child first. This never resubmits an
      // install; same-boot or already-submitted uncertainty remains blocking.
      for(const kind of ['package.install','package.receive'])
        for(const job of r.jobs.snapshot().filter(j=>j.kind===kind&&j.status==='interrupted')){
          try { await r.jobs.reconcile(job.id); }
          catch(error){if(error.code!=='OUTCOME_UNKNOWN')throw error;}
        }
      invariant(!r.jobs.snapshot().some(j => unresolved(j) && !startupJob(j) &&
        // Held work has never begun. Unresolved disk work keeps its resource
        // locks; only work sharing menu-injection resources can block the menu.
        j.status !== 'held' && locksMenu(j)),
        'RECOVERY_BLOCKED', 'Review unfinished operations in Activity first.');
      await r.store.transaction(state => {
        state.startup.pending = true;
        state.startup.native = { id: this.attempt, boot, status: 'starting', at: r.now(), service: null, error: null, services };
      }, reviewedRevision);
      if (recovery) {
        for (const job of r.jobs.snapshot().filter(j => startupJob(j) && unresolved(j))) {
          if (['held', 'queued'].includes(job.status)) await r.jobs.cancel(job.id);
          else if (job.status === 'interrupted') await r.jobs.reconcile(job.id);
          else throw new PmError('RECOVERY_BUSY', 'A prior startup worker is still active.');
        }
      }
      for (const service of services) {
        this.target = service;
        await r.store.transaction(state => { state.startup.native.service = service; });
        r.diagnostics?.record('startup', { status: 'native-step', target: service });
        try {
          const status = r.graph.states.get(service)?.status;
          if (recovery && ['failed', 'uncertain', 'blocked', 'ready'].includes(status)) await r.graph.reconcile(service);
          // Keep an uncertain native dispatch locked, but allow adoption of a
          // service that is already verified. Never queue a second injection.
          if(r.jobs.snapshot().some(j=>unresolved(j)&&j.status!=='held'&&locksMenu(j))){
            const evidence=await r.graph.definitions.get(service).inspect();
            invariant(evidence.ready,'COMPONENT_DISPATCH_HELD','This component cannot be launched until the previous native dispatch is resolved. Existing menu and services remain available.');
          }
          await r.graph.start(service);
        } catch(error){if(!isolatedService(service))throw error;isolate(service,error);}
      }
      // Verify the complete set again before promoting this startup configuration.
      for (const service of services) {
        if(issues.some(issue=>issue.service===service))continue;
        try{
          const definition = r.graph.definitions.get(service), evidence = await definition.inspect();
          invariant(await definition.verify(evidence),'STARTUP_READINESS_LOST',`${service} lost readiness before startup completed.`);
        }catch(error){
          if(!isolatedService(service))throw error;
          r.graph.invalidate(service,error);isolate(service,error);
        }
      }
      if(issues.length){
        await r.store.transaction(state=>{
          state.startup.pending=false;state.startup.safe=false;
          Object.assign(state.startup.native,{status:'failed',degraded:true,service:issues[0].service,error:issues[0].error,issues});
          this.history(state,'Optional features need attention',issues.map(issue=>`${issue.service}: ${issue.error.message}`).join(' '));
        });
        r.graph.safe=false;r.sequence++;r.notices?.observe();r.diagnostics?.observe();
        // Do not mark an incomplete configuration or binary trial known-good.
        return {completed:false,degraded:true,error:issues[0].error,issues};
      }
      await r.store.transaction(state => {
        state.startup.pending = false;state.startup.safe = false;
        Object.assign(state.startup.native, { status: 'ready', service: null, error: null });
        state.knownGood = clone(candidate);state.knownGood.developer.enabled = false;state.knownGood.developer.memoryWrites = false;
        state.knownGoodReceipt = { session: r.session, at: r.now(), firmware: r.identity.firmware || null, services };
        this.history(state, 'Configured native startup verified', 'The complete configured service set passed readiness checks.');
      });
      r.graph.safe = false;r.diagnostics?.record('startup', { status: 'native-ready' });
      // Binary promotion follows the complete native readiness check and its
      // durable configuration commit. A failed acknowledgement preserves the
      // launcher's unconfirmed trial; it does not undo live native services.
      r.updates?.confirmStartup();
    } catch (error) {
      failure = publicError(error);r.graph.safe = true;
      console.error('ProsperoMenu startup service failed:', this.target, failure.code, failure.message);
      try {
        await r.store.transaction(state => {
          state.startup.pending = false;state.startup.safe = true;
          state.startup.native = { id: this.attempt, boot, status: 'failed', at: r.now(), service: this.target, error: failure, services };
          this.history(state, 'Native startup needs attention', failure.message);
        });
      } catch (saveError) {
        // A failed failure-journal write cannot make startup safe to retry.
        r.jobs.halt(saveError);throw saveError;
      }
      r.diagnostics?.record('startup', { status: 'native-failed', target: this.target, code: failure.code });
    }
    r.sequence++;r.notices?.observe();r.diagnostics?.observe();
    return { completed: !failure, error: failure };
  }
  history(state, message, detail) {
    const r = this.runtime;state.history.push({ id: r.id(), session: r.session, at: r.now(), type: 'startup', undo: false, message, detail });
    state.history = state.history.slice(-256);
  }
}
