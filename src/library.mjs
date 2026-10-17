// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant, PmError, clone, publicError } from './errors.mjs';
import { localRequest } from './local-http.mjs';
const TITLE = /^(?:PPSA|PPSB|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS|MOUU)\d{5}$/;
export const sameService = (a, b) => a?.ready && b?.ready && a.boot === b.boot && a.pid === b.pid && a.generation === b.generation && a.version === b.version;
const text = (value, limit) => typeof value === 'string' && value.length <= limit && !/[\x00-\x1f]/.test(value);
function game(raw, version) {
  // Beta3 lists managed sources only. Beta4 adds installed PKGs and explicit
  // source-management fields. Never apply this fallback to an unknown schema.
  if (version === '1.7beta3') {
    invariant(typeof raw?.managed === 'boolean' && typeof raw?.image_backed === 'boolean' &&
      ['folder', 'image'].includes(raw.source_type), 'LIBRARY_RESPONSE', 'ShadowMount returned an incomplete beta3 game record.');
    raw = { ...raw, installed_pkg: false, can_manage_source: raw.managed };
  }
  invariant(raw && TITLE.test(raw.title_id) && text(raw.title_name, 512) && text(raw.path, 1024) &&
    text(raw.content_id, 128) && ['ps4', 'ps5', 'unknown'].includes(raw.platform) &&
    ['installed', 'mounted', 'installed_pkg', 'source_available', 'can_manage_source'].every(key => typeof raw[key] === 'boolean'),
    'LIBRARY_RESPONSE', 'ShadowMount returned an incomplete game record.');
  return { titleId: raw.title_id, name: raw.title_name, contentId: raw.content_id, platform: raw.platform,
    path: raw.path, sourceType: text(raw.source_type, 64) ? raw.source_type : 'unknown',
    installed: raw.installed, mounted: raw.mounted, installedPackage: raw.installed_pkg,
    sourceAvailable: raw.source_available, canMount: raw.can_manage_source && !raw.installed_pkg,
    bytes: Number.isSafeInteger(raw.app_db_size_bytes) && raw.app_db_size_bytes >= 0 ? raw.app_db_size_bytes : null };
}
export function installLibrary(runtime, { request = localRequest, timeout = 15000, catalog = null, native = null, log = () => {}, delay = ms => new Promise(r=>setTimeout(r,ms)) } = {}) {
  let refreshing = null, observedRevision = 0;
  let refreshId=0;
  const diagnostic=(stage,detail={})=>{try{log('ProsperoMenu library: '+JSON.stringify({refresh:refreshId,stage,...detail}));}catch{/* Logging must not block the library. */}};
  const failure=(source,error)=>{
    diagnostic(source+' failed',{code:error.code||'OPERATION_FAILED',message:String(error.message||error).replace(/[\x00-\x1f]/g,' ').slice(0,512)});
    const known=publicError(error);
    return {code:known.code,message:(source==='installed'?'Installed games: ':'ShadowMount: ')+(error instanceof PmError?known.message:'Could not read this game source. Refresh and submit runtime.log if it continues.'),action:known.action};
  };
  runtime.libraryState = { games: [], updatedAt: null, error: null, available: false };
  async function ready() {
    const evidence = await runtime.nativeServices.inspect('shadowmount');
    if(!evidence.ready)diagnostic('shadowmount not ready',{reasonCode:evidence.reasonCode||'SHADOWMOUNT_NOT_READY',absent:!!evidence.absent,busy:!!evidence.busy});
    invariant(evidence.ready, 'SHADOWMOUNT_NOT_READY', evidence.reason || 'Start and verify ShadowMount before opening the library.');
    return evidence;
  }
  async function call(route, body = {}, limit = 4 * 1024 * 1024, acceptedStatuses = [200, 400, 409, 500]) {
    const response = await request(10101, '/api/v1/' + route, { method: 'POST', body, maxBytes: limit, timeout, acceptedStatuses });
    const data = JSON.parse(response.text);
    invariant(data?.status === 0, 'SHADOWMOUNT_OPERATION', data?.error || 'ShadowMount did not complete the requested operation.');
    return data;
  }
  async function scan() {
    const before = await ready();
    diagnostic('shadowmount ready',{version:before.version});
    // include_size=false uses cached metadata; never walk multi-hundred-GB game
    // trees just to populate the phone list. Unknown size stays unknown.
    const data = await call('games', { include_size: false });
    diagnostic('shadowmount response',{count:data.count,records:Array.isArray(data.games)?data.games.length:null});
    invariant(Array.isArray(data.games) && data.games.length <= 8192 && data.count === data.games.length,
      'LIBRARY_RESPONSE', 'ShadowMount returned an invalid game list.');
    const games = data.games.map((item,index) => {
      try{return game(item,before.version);}catch(error){diagnostic('shadowmount record rejected',{index,version:before.version,fields:item&&typeof item==='object'?Object.keys(item).slice(0,40):[]});throw error;}
    }), ids = new Set(games.map(item => item.titleId));
    invariant(ids.size === games.length, 'LIBRARY_AMBIGUOUS', 'The library contains duplicate title identities.');
    invariant(sameService(before, await ready()), 'SERVICE_CHANGED', 'ShadowMount restarted while reading the library.');
    return { games, service: before };
  }
  async function refresh() {
    if (refreshing) return refreshing;
    const revision = observedRevision;
    refreshing = (async () => {
      try {
        refreshId++;diagnostic('refresh started');
        let installed=[], installedOK=false, mount=null;const warnings=[];
        // Independent sources: a locked/unreadable app.db must not hide the
        // ShadowMount list, just as a stopped ShadowMount must not hide PKGs.
        if(catalog){
          try{
            const records=await catalog();
            invariant(Array.isArray(records)&&records.length<=8192,'LIBRARY_RESPONSE','Installed-game catalog returned an invalid list.');
            // app.db also contains utility/homebrew records and display names
            // with line breaks. A single such row must not discard real games.
            const seen=new Set();let skipped=0,normalized=0;
            for(const item of records){
              if(!item||typeof item.titleId!=='string'||!TITLE.test(item.titleId)||seen.has(item.titleId)){skipped++;continue;}
              seen.add(item.titleId);
              const display=typeof item.name==='string'?item.name.replace(/[\x00-\x1f\x7f]/g,' ').trim().slice(0,512):'';
              const content=text(item.contentId,128)?item.contentId:'';
              if(display!==item.name||content!==item.contentId)normalized++;
              installed.push({titleId:item.titleId,name:display||item.titleId,contentId:content,
                version:typeof item.version==='string'&&(/^(PPS|MOUU)/.test(item.titleId)?/^\d{2}\.\d{3}\.\d{3}$/:/^\d{2}\.\d{2}$/).test(item.version)?item.version:null,
                bytes:Number.isSafeInteger(item.bytes)&&item.bytes>=0?item.bytes:null});
            }
            installedOK=true;diagnostic('installed ready',{count:installed.length,skipped,normalized});
          }catch(error){warnings.push(failure('installed',error));}
        }
        try{mount=await scan();}catch(error){warnings.push(failure('shadowmount',error));if(!catalog)throw error;}
        const warning=warnings.length?{code:warnings[0].code,message:warnings.map(w=>w.message).join(' '),action:warnings[0].action}:null;
        if(!installedOK&&!mount)throw new PmError('LIBRARY_UNAVAILABLE',warning?.message||'No game source could be read. Refresh the library.');
        const merged=new Map();
        for(const item of installed){
          invariant(TITLE.test(item.titleId)&&text(item.name,512)&&text(item.contentId,128),'LIBRARY_RESPONSE','Installed-game catalog returned an invalid game.');
          merged.set(item.titleId,{...item,platform:item.titleId.startsWith('PPS')?'ps5':'ps4',path:'',sourceType:'installed',installed:true,mounted:false,installedPackage:true,sourceAvailable:false,canMount:false});
        }
        for(const item of mount?.games||[])merged.set(item.titleId,{...item,version:merged.get(item.titleId)?.version??null,installed:item.installed||merged.has(item.titleId)});
        const result={games:[...merged.values()].sort((a,b)=>a.name.localeCompare(b.name))};
        // A mount observation made while this scan was in flight is newer than
        // this response. Never put its old action back into the open menu.
        if (revision !== observedRevision) return page();
        runtime.libraryState = { games: result.games, updatedAt: runtime.now(), error: warning, available: true };
        diagnostic('refresh finished',{installed:installed.length,shadowmount:mount?.games.length??null,total:result.games.length,partial:!!warning});
        return page();
      } catch (error) {
        if (revision !== observedRevision) return page();
        diagnostic('refresh failed',{code:error.code||'OPERATION_FAILED'});
        runtime.libraryState.available = false; runtime.libraryState.error = publicError(error); throw error;
      } finally { runtime.sequence++; refreshing = null; }
    })();
    return refreshing;
  }
  async function target(input, { jobId = null } = {}) {
    invariant(input && TITLE.test(input.titleId) && typeof input.mounted === 'boolean', 'LIBRARY_INPUT', 'Choose a game and a mount state.');
    const current = await scan(), item = current.games.find(item => item.titleId === input.titleId);
    invariant(item?.canMount && item.sourceAvailable, 'LIBRARY_SOURCE', 'This game source is unavailable or is an installed package.');
    // Mounting is the one feature that needs its own capability: a ShadowMount
    // build that only lists games still fills the Games page.
    invariant((current.service.capabilities || []).includes('mount_game'), 'SHADOWMOUNT_MOUNT_UNSUPPORTED',
      'This ShadowMount build cannot mount game sources. Update it to a version that exposes mount_game.');
    if (input.path) invariant(input.path === item.path && input.contentId === item.contentId, 'LIBRARY_CHANGED', 'The game source changed after this request.');
    if (input.service) invariant(sameService(input.service, current.service), 'SERVICE_CHANGED', 'ShadowMount restarted after this request.');
    if (typeof input.priorMounted === 'boolean') invariant(input.priorMounted === item.mounted,
      'HISTORY_CONFLICT', 'The current mount no longer matches the reviewed change.');
    const accepted = { titleId: item.titleId, path: item.path, contentId: item.contentId, mounted: input.mounted,
      priorMounted: typeof input.priorMounted === 'boolean' ? input.priorMounted : item.mounted, service: current.service };
    if (input.undoOf !== undefined) accepted.undoOf = clone(input.undoOf);
    runtime.changes.guardJobUndo(accepted, 'library.mount', { jobId });return accepted;
  }
  runtime.jobs.register('library.mount', {
    validate: target, guard: (input, state) => runtime.changes.guardJobUndo(input, 'library.mount', { state }),
    resources: () => ['shadowmount-library'], timeout: timeout + 15000, cancellable: false,
    async run(input, context) {
      try { await target(input, { jobId: context.id }); }
      catch (error) { error.noSideEffects = true; throw error; }
      if (input.priorMounted === input.mounted) return observe(input);
      await context.progress({ completed: 0, total: 1, phase: input.mounted ? 'Mounting game source' : 'Unmounting game source' });
      try { runtime.changes.guardJobUndo(input, 'library.mount', { jobId: context.id }); }
      catch (error) { error.noSideEffects = true;throw error; }
      // Upstream's exact pinned implementation checks active games/sleep and
      // returns EBUSY before changing a live game's mount. No override is sent.
      const result = await call(input.mounted ? 'games/mount' : 'games/unmount', { title_id: input.titleId }, 65536);
      diagnostic('mount response',{titleId:input.titleId,requested:input.mounted,returned:result.mounted});
      invariant(result.title_id === input.titleId && result.mounted === input.mounted, 'MOUNT_RESPONSE', 'The mount response does not match this request.');
      return observe(input);
    },
    verify: async (result, input) => result?.completed === true && result.titleId === input.titleId &&
      result.mounted === input.mounted && sameService(input.service, await ready()),
    reconcile: job => {
      const boot=native?.systemProbe?.().boot, previous=job.args.service?.boot;
      if(boot&&previous&&boot!==previous)return {expired:true,previousBoot:previous,boot};
      return observe(job.args,true);
    },
    verifyExpired:async(result,job)=>result.expired===true&&result.previousBoot===job.args.service?.boot&&
      Boolean(result.boot)&&result.boot===native?.systemProbe?.().boot&&result.boot!==result.previousBoot
  });
  async function observe(input, reconciliation = false) {
    const result = await scan(), item = result.games.find(item => item.titleId === input.titleId);
    invariant(sameService(input.service, result.service) && item?.path === input.path && item.contentId === input.contentId,
      'MOUNT_IDENTITY', 'The service or game identity changed before mount verification.');
    if (reconciliation && item.mounted !== input.mounted && item.mounted === input.priorMounted) return { absent: true };
    invariant(item.mounted === input.mounted, 'MOUNT_UNCONFIRMED', 'The desired mount state has not been verified.');
    diagnostic('mount verified',{titleId:item.titleId,mounted:item.mounted,reconciliation});
    // Publish the independently verified item before completing the job. This
    // also covers no-op and reconciled jobs without requiring a manual refresh.
    publishItem(item);
    return { completed: true, titleId: input.titleId, mounted: input.mounted, message: input.mounted ? 'Game source mounted and verified.' : 'Game source unmounted and verified.' };
  }
  function publishItem(item) {
    const previous = runtime.libraryState.games.find(g => g.titleId === item.titleId);
    const next={...item, version:previous?.version??null, installed:item.installed || !!previous?.installed};
    if(JSON.stringify(previous)===JSON.stringify(next))return;
    const games = runtime.libraryState.games.filter(g => g.titleId !== item.titleId);
    games.push(next);
    runtime.libraryState = {...runtime.libraryState, games:games.sort((a,b)=>a.name.localeCompare(b.name)),
      available:true, updatedAt:runtime.now()};
    observedRevision++;runtime.sequence++;
    diagnostic('menu state published',{titleId:item.titleId,mounted:item.mounted});
  }
  async function refreshGame(titleId) {
    invariant(TITLE.test(titleId),'LIBRARY_INPUT','Choose a game.');
    const revision=observedRevision, result=await scan();
    if(revision!==observedRevision)return;
    const item=result.games.find(g=>g.titleId===titleId);
    invariant(item,'LIBRARY_CHANGED','Game source is no longer listed. Refresh the library.');
    publishItem(item);
  }
  if(native?.appLaunch && native?.appLaunchPoll)runtime.jobs.register('library.launch',{
    async validate(input){
      invariant(input&&TITLE.test(input.titleId),'LIBRARY_INPUT','Choose a game to play.');
      await refresh();const item=runtime.libraryState.games.find(g=>g.titleId===input.titleId);
      invariant(item&&(item.installed||item.canMount&&item.sourceAvailable),'GAME_UNAVAILABLE','This game is not installed or its source is disconnected.');
      // A process from a suspended/background title must not block launching
      // another one: the console's own launcher switches applications. Mounts
      // and cheats still require exclusivity and keep their own guards.
      return {titleId:item.titleId,name:item.name,contentId:item.contentId,path:item.path};
    },
    resources:()=>['shadowmount-library','native-dispatch'],timeout:45000,cancellable:false,
    async run(input,context){
      await refresh();const item=runtime.libraryState.games.find(g=>g.titleId===input.titleId);
      invariant(item&&item.path===input.path&&item.contentId===input.contentId,'LIBRARY_CHANGED','The selected game changed. Refresh the game list.');
      if(item.canMount&&!item.mounted){
        await context.progress({completed:0,total:2,phase:'Mounting game'});
        const mount=await target({...input,mounted:true});
        await runtime.jobs.handlers.get('library.mount').run(mount,context);
      }
      await context.progress({completed:1,total:2,phase:'Starting game'});
      native.appLaunch(context.id,input.titleId);
      for(let i=0;i<120;i++){
        const r=native.appLaunchPoll(context.id);
        if(r){invariant(r.request===context.id&&r.titleId===input.titleId&&r.currentBoot,'GAME_RECEIPT','Game-launch receipt does not match this request.');
          if(r.stage===3)return {completed:true,titleId:r.titleId,appId:r.appId,message:r.message};
          if(r.stage===4||r.stage===5){const code=r.result?'0x'+(r.result>>>0).toString(16):'';const error=new PmError('GAME_LAUNCH',r.message+(code&&!String(r.message).includes(code)?' ('+code+')':''));if(r.stage===4)error.noSideEffects=true;throw error;}}
        await delay(200);
      }
      throw new PmError('GAME_LAUNCH_PENDING','The console has not confirmed the launch. Check the dashboard before trying again.');
    },
    async verify(result,input){return result?.completed&&result.titleId===input.titleId&&result.appId>=0;},
    async reconcile(job){const r=native.appLaunchPoll(job.id);if(r?.stage===3&&r.currentBoot&&r.titleId===job.args.titleId)return {completed:true,titleId:r.titleId,appId:r.appId,message:r.message};throw new PmError('GAME_LAUNCH_PENDING','Check the dashboard; this launch will not be repeated automatically.');}
  });
  function page({ search = '', offset = 0 } = {}) {
    invariant(typeof search === 'string' && search.length <= 128 && Number.isSafeInteger(offset) && offset >= 0,
      'LIBRARY_QUERY', 'Invalid library search.');
    const term = search.trim().toLowerCase();
    const matches = runtime.libraryState.games.filter(item => !term || (item.name + ' ' + item.titleId).toLowerCase().includes(term));
    return { games: clone(matches.slice(offset, offset + 100)), total: matches.length, offset,
      updatedAt: runtime.libraryState.updatedAt, available: runtime.libraryState.available, error: runtime.libraryState.error };
  }
  runtime.library = { refresh, refreshGame, page, scan, ready, call };
  runtime.capability('shadowmount', true, 'ShadowMount game library and verified mount controls; storage conversion jobs are still being integrated.');
  return runtime.library;
}
