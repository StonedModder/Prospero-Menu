// SPDX-License-Identifier: GPL-3.0-or-later
// Controller presentation uses the same commands, validation and durable jobs as
// the phone. No privileged operation is implemented inside the renderer.
import { invariant } from './errors.mjs';
import { telemetryText } from './telemetry.mjs';
import { describeBootAnimation, BOOTANIM_FOLDER } from './boot-animation.mjs';
import { packageDeckRows } from './deck-packages.mjs';
import { transferRows } from './deck-transfers.mjs';
import { KEYBOARDS, editDownload, suggestedName, downloadRows } from './deck-download.mjs';
import { startupPayloadRows } from './deck-startup.mjs';
import { payloadDownloadsRows } from './deck-downloads.mjs';
import { continuePayloadLoad } from './deck-payload-load.mjs';
import { fileManagerFrame,openFileManager,refreshFileManager } from './deck-file-browser.mjs';
import { startAutoload, autoloadRows, autoloadEdit, openSource, openInternal, refreshSource, findPayload } from './deck-autoload.mjs';
import { openElfUpdater, elfUpdaterRows, updaterNotes, readStartup } from './deck-elf-updater.mjs';
import { loadDumper, dumperRows, dumperStart, dumperStop } from './deck-dumper.mjs';
import { Dashboard } from './dashboard.mjs';
import { ProsperoRoot } from './overlays/deck-root.mjs';

export const DECK_WIRE = Object.freeze({ magic: 0x54414344, version: 13, bytes: 19024, rows: 32, rowBytes: 292, theme: 18776, logo: 18824, videoSerial: 19016 });
// ProsperoMenu: the menu has no Cheats or Saved games pages. Page 5 remains only
// for ProsperoMenu's separate dashboard cheats window.
const pages = ['Home', 'Payloads', 'Games', 'Transfers', 'Settings'];
// Longest prefix of a UTF-8 buffer that fits in `limit` bytes without
// splitting a code point.
function utf8Prefix(input, limit) {
  let length = Math.min(input.length, Math.max(0, limit));
  while (length > 0 && length < input.length && (input[length] & 0xc0) === 0x80) length--;
  return length;
}
function text(buffer, offset, bytes, value) {
  const input = Buffer.from(String(value ?? '').replace(/[\x00-\x1f\x7f]/g, ' '));
  let length = Math.min(input.length, bytes - 1);
  // Do not split a UTF-8 code point at a fixed-width wire boundary.
  while (length < input.length && length && (input[length] & 0xc0) === 0x80) length--;
  input.copy(buffer, offset, 0, length);
}
export function encodeDeck(frame) {
  invariant(frame.rows.length <= DECK_WIRE.rows, 'DECK_LIMIT', 'Too many deck rows.');
  const accent = ['mint', 'amber', 'ice'].indexOf(frame.accent ?? 'mint');
  invariant(accent >= 0, 'DECK_ACCENT', 'Unknown deck accent.');
  const out = Buffer.alloc(DECK_WIRE.bytes);
  out.writeUInt32LE(DECK_WIRE.magic, 0); out.writeUInt16LE(DECK_WIRE.version, 4);
  out.writeUInt16LE((frame.ready ? 1 : 0) | (frame.ipVisible ? 2 : 0) | (frame.canBack ? 4 : 0) | (accent << 3) | (frame.grid ? 32 : 0) | (frame.reducedMotion ? 64 : 0) | (frame.keyboard ? 128 : 0) | (frame.browser ? 256 : 0), 6);
  out.writeUInt32LE(frame.sequence, 8); out.writeUInt16LE(frame.page, 12); out.writeUInt16LE(frame.rows.length, 14);
  // ProsperoMenu windowing: the frame ships a moving window of
  // rows. 0xFFFF focus means the deck keeps its own focused item.
  out.writeUInt16LE(Math.max(0, frame.windowStart || 0), 16);
  out.writeUInt16LE(Math.min(65535, frame.total ?? frame.rows.length), 18);
  out.writeUInt16LE(frame.focus ?? 0xFFFF, 20);
  text(out, 24, 64, frame.title); text(out, 88, 160, frame.subtitle);
  text(out, 248, 160, frame.footer); text(out, 408, 64, frame.ip);
  frame.rows.forEach((row, index) => {
    const at = 472 + index * DECK_WIRE.rowBytes;
    // token is a full 32-byte hex identifier, not a C string.
    Buffer.from(row.token).copy(out, at, 0, 32);
    // ProsperoMenu v10 rows: a right-aligned value rides after the label's
    // terminator; headers are never enabled; adjustable rows take Left/Right.
    const header = row.kind === 'header', value = header || row.value == null || row.value === '' ? null : String(row.value);
    if (value !== null) {
      const v = Buffer.from(value.replace(/[\x00-\x1f\x7f]/g, ' '));
      const valueBytes = utf8Prefix(v, 31);
      const label = Buffer.from(String(row.label ?? '').replace(/[\x00-\x1f\x7f]/g, ' '));
      const labelBytes = utf8Prefix(label, 95 - valueBytes - 1);
      label.copy(out, at + 32, 0, labelBytes); v.copy(out, at + 32 + labelBytes + 1, 0, valueBytes);
    } else text(out, at + 32, 96, row.label);
    text(out, at + 128, 160, row.detail);
    out.writeUInt32LE(header ? 8 : (row.enabled ? 1 : 0) | (row.on ? 2 : 0) | (row.adjust && row.enabled ? 4 : 0) | (value !== null ? 16 : 0), at + 288);
  });
  out.writeUInt32LE(frame.metricsSequence || 1, 9816);text(out, 9820, 192, frame.metrics);
  const input = frame.input;
  if (input) {
    invariant(input.events.length <= 128, 'DECK_INPUT_LIMIT', 'Too many controller events.');
    for (const [offset, value] of [[10016, input.epoch], [10020, input.serial], [10024, input.active], [10028, input.captured ? 1 : 0], [10036, input.connection], [10040, input.events.length]]) out.writeUInt32LE(value, offset);
    out.writeInt32LE(input.owner, 10032);out.writeBigUInt64LE(BigInt(Math.floor(input.deadline)), 10048);
    out.writeBigUInt64LE(BigInt(Math.ceil(input.captureAt || 0)), 10056);
    input.events.forEach((e, i) => { const at = 10064 + i * 24;out.writeUInt32LE(e.buttons, at);out.writeInt32LE(e.handle, at + 4);
      out.writeUInt32LE(e.connection, at + 8);out.writeUInt32LE(e.sequence, at + 12);out.writeBigUInt64LE(BigInt(Math.floor(e.time)), at + 16); });
  }
  const notice = frame.notices?.current;
  out.writeUInt32LE(frame.notices?.sequence || 1, 13136);
  if (notice && notice.remainingMs > 0) {
    out.writeUInt32LE(Math.min(9000, Math.ceil(notice.remainingMs)), 13140);
    out.writeUInt32LE(notice.level === 'attention' ? 2 : 1, 13144);out.writeUInt32LE(notice.count, 13148);
    text(out, 13152, 96, notice.title + (notice.count > 1 ? ` (${notice.count})` : ''));
    text(out, 13248, 192, notice.detail);
  }
  if(frame.browser){
    invariant(frame.browser.panes.length===2&&frame.browser.panes.every(p=>p.entries.length===12),'DECK_BROWSER','Invalid browser panes.');
    out.writeUInt32LE(frame.browser.active,13440);
    frame.browser.panes.forEach((p,i)=>{text(out,13444+i*160,160,p.path);p.entries.forEach((r,j)=>{const at=13924+(i*12+j)*164;text(out,at,96,r.label);text(out,at+96,64,r.detail);out.writeUInt32LE((r.enabled?1:0)|(r.on?2:0)|(r.directory?4:0),at+160);});});
    text(out,13764,160,frame.browser.status);
  }
  const dashboard=frame.dashboard;
  if(dashboard){out.writeUInt32LE(dashboard.enabled?1:0,17864);out.writeUInt32LE(dashboard.epoch,17868);out.writeUInt32LE(dashboard.ack,17872);
    invariant(dashboard.items.length<=32,'DASHBOARD_LIMIT','Too many dashboard records.');out.writeUInt32LE(dashboard.items.length,17876);
    dashboard.items.forEach((g,i)=>{text(out,17880+i*28,10,g.titleId);text(out,17890+i*28,16,g.version);});}
  // ProsperoSettings theme: serial then RGBA8 colours (accent, background,
  // text, muted, separator, scrollbar, edge, focus fill). Serial 0 = built-in.
  const theme = frame.theme;
  if (theme?.serial) {
    invariant(Array.isArray(theme.colors) && theme.colors.length === 10, 'DECK_THEME', 'Invalid theme.');
    out.writeUInt32LE(theme.serial >>> 0, DECK_WIRE.theme);
    theme.colors.forEach((c, i) => out.writeUInt32LE(c >>> 0, DECK_WIRE.theme + 4 + i * 4));
  }
  // v12: the replaceable header logo. The deck loads it with the validated
  // image widget; an empty path keeps the drawn mark, so a host without the
  // image ABI still renders a header.
  const logo = frame.logoPath || '';
  if (logo) {
    invariant(typeof logo === 'string' && !/[\x00-\x1f\x7f]/.test(logo) && Buffer.byteLength(logo) < 192, 'DECK_LOGO', 'Invalid logo path.');
    out.write(logo, DECK_WIRE.logo, Buffer.byteLength(logo), 'utf8');
  }
  // v13: boot animation frame counter. The deck reloads the frame image
  // whenever this changes, giving frame-accurate animation.
  out.writeUInt32LE((frame.videoSerial || 0) >>> 0, DECK_WIRE.videoSerial);
  return out;
}

export class ConsoleDeck {
  constructor(runtime, { schedule = setInterval, cancel = clearInterval } = {}) {
    this.runtime = runtime; this.page = 0; this.offset = 0; this.sequence = 0;
    this.signature = ''; this.current = null; this.requests = new Map(); this.tail = Promise.resolve();
    this.notice = ''; this.confirmation = null;
    this.metrics = ''; this.metricsSequence = 1;
    this.schedule=schedule;this.cancel=cancel;this.libraryTimer=null;this.visible=true;
    this.autoloadView=null;this.dumperView=null;this.elfView=null;
    this.dashboard=new Dashboard(runtime);
    // ProsperoMenu root list, overlay pages and ProsperoSettings.
    this.prospero=new ProsperoRoot(runtime);
  }
  // ProsperoMenu's own pages are idle when Home is showing with no sub-view open;
  // that is where ProsperoMenu's root list and pages are drawn.
  pmIdle(){
    return this.page===0&&!this.managerView&&!this.payloadDownloads&&!this.textInput&&!this.downloadView&&!this.startupPayloadView&&!this.confirmation&&!this.fileView&&!this.importJob&&!this.packageView&&!this.saveView&&!this.historyView&&!this.startupReview&&!this.loadoutPreview&&!this.autoloadView&&!this.dumperView&&!this.elfView;
  }
  openGameCheats(titleId){
    this.page=5;this.offset=0;this.confirmation=null;this.cheatPack=null;this.cheatTitle=titleId;
    for(const key of ['managerView','payloadDownloads','textInput','downloadView','startupPayloadView','fileView','importJob','packageView','loadoutPreview','saveView','historyView','startupReview','gameView'])this[key]=null;
    this.notice='Cheats for '+titleId;this.refreshCheats();
  }
  async openCheats(){
    this.page=5;this.offset=0;this.cheatPack=null;this.cheatTitle=null;this.notice='';
    const state=this.runtime.snapshot(),game=state.game;
    // Select a library entry only; never enable a cheat just by opening the tab.
    const entry=game?.titleId&&game?.version&&state.cheats?.find(pack=>
      pack.titleId===game.titleId&&pack.version===game.version&&pack.count>0);
    if(entry){
      try{
        const pack=await this.runtime.execute('cheat.inspect',{sha256:entry.sha256});
        const current=this.runtime.snapshot().game;
        if(current&&['titleId','version','pid','generation'].every(key=>current[key]===game[key])&&
          pack.titleId===game.titleId&&pack.version===game.version&&pack.cheats?.length)this.cheatPack=pack;
      }catch{ /* A missing or unreadable pack leaves the normal library available. */ }
    }
    this.refreshCheats();
  }
  setVisible(visible){this.visible=visible;this.updateLibraryLoop();this.prospero.setVisible(visible);}
  stop(){if(this.libraryTimer!==null)this.cancel(this.libraryTimer);this.libraryTimer=null;}
  updateLibraryLoop(){
    if(this.page!==2||!this.visible||!this.runtime.library){this.stop();return;}
    if(this.libraryTimer===null){
      this.libraryTimer=this.schedule(()=>{
        if(this.page!==2||!this.visible){this.stop();return;}
        this.refreshLibrary(true);
      },3000);
      this.libraryTimer?.unref?.();
    }
  }
  frame() {
    const runtime = this.runtime, state = runtime.snapshot({internal:true});
    this.updateLibraryLoop();
    const syncingCheats=state.jobs.some(j=>j.kind==='cheat.sync'&&!['completed','failed','cancelled'].includes(j.status));
    // Import commits files individually. Keep the native list stable while the
    // download runs, then publish the finished catalog once, not once per file.
    if(!this.nativeCheatCatalog||!syncingCheats)this.nativeCheatCatalog=state.cheats||[];
    const cheatCatalog=this.nativeSettings?this.nativeCheatCatalog:(state.cheats||[]);
    const items = [], row = (label, detail, command = null, args = {}, extra = {}) =>
      items.push({ label, detail, enabled: Boolean(command), command, args, ...extra });
    if(this.managerView?.job&&!this.managerRefreshing){const v=this.managerView,j=state.jobs.find(x=>x.id===v.job);
      if(j&&['completed','failed','cancelled'].includes(j.status)&&v.refreshed!==j.id){v.refreshed=j.id;this.managerRefreshing=true;
        refreshFileManager(runtime,v).catch(e=>{v.message=e.message;}).finally(()=>{this.managerRefreshing=false;});}}
    const feature = name => state.capabilities.find(x => x.id === name)?.available;
    const pm=this.prospero,pmIdle=this.pmIdle();
    // Returning from a ProsperoMenu page opened from a ProsperoMenu page goes back there.
    if(pmIdle&&pm.suspended){pm.location=pm.suspended;pm.suspended=null;}
    if(pmIdle&&pm.returnFocusId){pm.toRoot(pm.returnFocusId);pm.returnFocusId=null;}
    if(pm.rootFocus!=null&&(pm.active()||!pmIdle||Date.now()-pm.rootFocusAt>3000))pm.rootFocus=null;
    let pmView=null;
    if(this.textInput){
      row(KEYBOARDS[this.textInput.page],this.textInput.value.slice(-145) || ' ', '$keyboard');
    } else if (this.confirmation) {
      row('Confirm: ' + this.confirmation.label, this.confirmation.detail, '$confirm');
      row('Cancel', 'Return without making changes', '$cancel');
    } else if(pmIdle&&pm.active()){
      pmView=pm.view(state);items.push(...pmView.rows);
    } else if(this.managerView){
      items.push(...fileManagerFrame(this.managerView,state.jobs).rows);
    } else if(this.payloadDownloads){
      items.push(...payloadDownloadsRows(state,this.payloadDownloads,runtime.componentsByHash));
    } else if(this.downloadView){
      items.push(...downloadRows(state,this.downloadView,runtime.componentsByHash));
    } else if (this.startupPayloadView) {
      items.push(...startupPayloadRows(state, this.startupPayloadView, runtime.componentHashes));
    } else if (this.importJob) {
      const job = state.jobs.find(j => j.id === this.importJob.id);
      row(job?.status === 'completed' ? 'File ready' : job?.status === 'failed' ? 'Could not prepare file' : 'Preparing file',
        job?.error?.message || job?.result?.message || job?.progress?.phase || (this.importJob.kind==='package'?'Checking the selected package without copying it.':'Waiting to copy and check the selected file.'));
      if (job?.status === 'completed') row(this.importJob.kind === 'package' ? 'Continue to installation' : this.importJob.kind === 'cheat' ? 'Open cheats' : 'Open payloads',
        'The original file has not been changed.', '$import-open', { kind: this.importJob.kind, id: job.result.id });
      if (job && ['queued','running','held'].includes(job.status)) row(this.importJob.kind==='package'?'Cancel check':'Cancel copy', 'Keep the original file.', 'job.cancel', { id: job.id });
      row('Back to files', 'You can leave this screen while preparation runs.', '$import-back');
    } else if (this.fileView) {
      const view = this.fileView;
      if(view.kind!=='payload')row('Refresh', 'Check connected drives and files again.', '$browse', { kind: view.kind, source: view.source });
      if(view.usbMissing)row('No USB drive connected','Connect a USB drive, then choose Refresh. Console storage is still available.');
      for (const item of view.entries) row((item.directory ? '[Folder] ' : '') + item.name,
        item.directory ? 'Open folder' : Math.ceil(item.bytes / 1048576) + ' MiB · ' + (view.kind==='cheat'?'Import this cheat file':view.kind==='package'?'Press X to install directly. Keep USB connected.':view.kind==='payload'?'Press X to check and load this payload.':'Copy and check before use'),
        item.directory ? '$browse' : 'job.enqueue', item.directory ? { kind: view.kind, source: item.source } :
          { kind: view.kind==='package'?'package.receive':'file.import', input: { kind: view.kind, source: item.source, bytes: item.bytes, modified: item.modified } },
        item.directory ? {} : view.kind==='package'?{packageWorkflow:true}:view.kind==='payload'?{payloadWorkflow:true}:{ confirm: true, fileImport: view.kind });
      if(view.kind==='payload')row('Refresh', 'Check connected drives and files again.', '$browse', { kind: view.kind, source: view.source });
      if (!view.entries.length) row('No matching files here', 'Connect a USB drive or open a folder containing the selected file type.');
      row(view.source ? 'Up one folder' : 'Back', '', view.source ? '$browse' : '$files-exit', { kind: view.kind, source: view.parent });
    } else if (this.packageView) {
      items.push(...packageDeckRows(state, this.packageView, state.jobs));
    } else if (this.startupReview) {
      const plan = this.startupReview;
      for (const service of plan.services) row(service.id, service.status === 'adopt' ? 'Reuse after readiness check' : service.status === 'start' ? 'Start after dependencies' : service.reason);
      for (const blocker of plan.blockers) row('Needs attention', blocker);
      row('Continue reviewed startup', plan.message, plan.available && !state.recovery?.running ? 'startup.resume' : null, { proof: plan.proof }, { confirm: true });
      row('Review again', 'Refresh existing service evidence', '$startup-review');
      row('Back', 'Return to the command deck', '$startup-back');
    } else if (this.historyView) {
      if (this.historyView.id && !state.history.some(entry => entry.id === this.historyView.id)) {
        row('Change no longer available', 'The session history has advanced. Return to the list.');
        row('Back to history', '', '$history-back');
      } else if (this.historyView.id) {
        const plan = runtime.changes.preview({ id: this.historyView.id });
        row(plan.message, plan.available ? plan.detail : plan.reason);
        for (const change of plan.changes) row(change, '');
        row(plan.label, plan.detail, plan.available ? 'history.undo' : null, { id: plan.id, proof: plan.proof }, { confirm: true });
        row('Back to history', '', '$history-back');
      } else {
        // An operation without an undo plan still says how it ended and why;
        // the generic "no reversible change" reason hid every failure here.
        const outcome = { completed: 'Completed', failed: 'Failed', interrupted: 'Interrupted', uncertain: 'Outcome unknown', cancelled: 'Cancelled', held: 'Held' };
        for (const entry of [...state.history].reverse()) row(entry.message, entry.undo.available ? 'Review undo' :
          entry.type === 'job' ? [outcome[entry.status] || entry.status, entry.detail].filter(Boolean).join(' · ') : entry.undo.reason,
          entry.undo.proof ? '$history-open' : null, { id: entry.id });
        if (!state.history.length) row('No session changes', 'Changes and operation results will appear here');
      }
    } else if (this.loadoutPreview) {
      const plan = this.loadoutPreview;
      row(plan.profile.name, `${plan.profile.titleId} · ${plan.profile.version}`);
      row('Apply loadout', plan.canApply ? 'Run checked steps in order; completed steps remain if a later one fails.' : 'Resolve the listed issues first',
        plan.canApply && !state.safe ? 'job.enqueue' : null, { kind: 'loadout.apply', input: plan.accepted }, { confirm: true });
      for (const step of plan.steps) row(step.label, step.existing ? 'Reuse recorded operation' : 'Start after earlier steps succeed');
      for (const [key, enabled] of Object.entries(plan.profile.overlay)) row(key.toUpperCase() + ' overlay', enabled ? 'On for this game session' : 'Off for this game session');
      for (const issue of plan.issues) row(issue.label, issue.message);
      row('Back to games', 'Return without starting anything', '$loadout-back');
    } else if (this.saveView) {
      const list = runtime.saves.list({ offset: this.saveView.offset, limit: 3 });
      if (this.saveView.job) {
        const job=state.jobs.find(j=>j.id===this.saveView.job);
        const checking=state.saves?.recovering?.includes(job?.id);
        const labels={completed:'Backup saved',failed:'Backup failed',cancelled:'Backup cancelled',held:'Backup paused',interrupted:'Backup needs checking',uncertain:'Backup needs checking',queued:'Backup waiting',cancelling:'Cancelling backup',running:'Backing up save'};
        row(checking?'Checking backup result':labels[job?.status]||'Backup no longer listed',
          job?.error?.message || job?.result?.message || job?.progress?.phase || 'Preparing a copy of the save.');
        if(job?.error?.action)row('What to do next',job.error.action);
        if(job?.status==='interrupted'&&!checking)row('Check backup result','Check the existing backup without starting another export.','job.reconcile',{id:job.id});
        if(job?.status==='held')row('Resume backup','Continue this paused backup.','job.resume',{id:job.id},{confirm:true});
        if(job?.result?.output)row('Saved file',job.result.output);
        if(job?.result?.ftpPath)row('Copy it to your computer',`FTP ${state.identity.ip}:2122 → ${job.result.ftpPath}`);
        row('Back to saved games','','$save-list');
      } else if (this.saveView.usb) {
        row('USB backup','Connect a USB drive to the PS5, then refresh.');
        row('Refresh USB drives','Read connected drives again.','job.enqueue',{kind:'save.refresh',input:{}});
        for(const volume of list.volumes)row('Back up to USB '+volume.destination,
          `${volume.path}/ProsperoMenu/Saves · ${Math.floor(volume.freeBytes/1048576)} MiB free`,
          feature('saves')&&!state.safe&&state.game?.titleId!==this.saveView.selected?.titleId?'job.enqueue':null,
          {kind:'save.export',input:{...this.saveView.selected,destination:volume.destination,volume:volume.stamp}},{confirm:true});
        if(!list.volumes.length)row('No USB drive detected','Plug in a writable USB drive and choose Refresh USB drives.');
        row('Back to backup options','','$save-destinations');
      } else if (this.saveView.selected) {
        const item = this.saveView.selected;
        row(runtime.libraryState?.games.find(g=>g.titleId===item.titleId)?.name || item.titleId, `${item.slot} · User ${item.user}`);
        const blocked=!feature('saves')?'Save backup service is unavailable.':state.safe?'Finish service recovery before backing up saves.':state.game?.titleId===item.titleId?'Close this game before backing up its save.':'';
        if(blocked)row('Before backing up',blocked);
        row('Back up to console',blocked||'Save a decrypted copy under /data/ProsperoMenu/share/saves. The exact file path appears when finished.',
          feature('saves') && !state.safe && state.game?.titleId !== item.titleId ? 'job.enqueue' : null,
          {kind:'save.export',input:{...item,destination:0,volume:''}},{confirm:true});
        row('Back up to USB','Choose a USB drive connected to this PS5.','$save-usb');
        row('Back to saves', 'Choose another save', '$save-back');
      } else {
        const scan=state.jobs.filter(j=>j.kind==='save.refresh').at(-1);
        if(scan && scan.status!=='completed')row(['failed','interrupted','uncertain'].includes(scan.status)?'Save scan needs attention':scan.status==='cancelled'?'Save scan cancelled':scan.status==='held'?'Save scan paused':'Finding saved games',scan.error?.message || scan.result?.message || scan.progress?.phase || 'Please wait while console saves are scanned.');
        if(list.updatedAt&&!list.total)row('No saved games found', 'Play and save a game on this console, then refresh.');
        row('Refresh saves', list.updatedAt ? `${list.total} saves · ${list.skipped} skipped entries` : 'Read saved games and connected USB drives',
          feature('saves') && !state.safe && (!scan || ['completed','failed','cancelled'].includes(scan.status)) ? 'job.enqueue' : null, { kind: 'save.refresh', input: {} });
        for (const item of list.entries) row(runtime.libraryState?.games.find(g=>g.titleId===item.titleId)?.name||item.titleId, `${item.titleId} · ${item.slot} · User ${item.user}`, '$save-open', item);
        if (list.offset) row('Previous saves', '', '$save-page', { offset: Math.max(0, list.offset - 3) });
        if (list.offset + 3 < list.total) row('More saves', '', '$save-page', { offset: list.offset + 3 });
      }
    } else if (this.autoloadView) {
      // Pages opened from an overlay draw over Home, so they come before it.
      items.push(...autoloadRows(this.autoloadView));
    } else if (this.elfView) {
      items.push(...elfUpdaterRows(runtime, this.elfView, state));
    } else if (this.dumperView) {
      items.push(...dumperRows(this.dumperView));
    } else if (this.page === 0) {
      // ProsperoMenu root: every entry comes from /data/ProsperoMenu/overlays;
      // ProsperoSettings is the only fixed entry and always the last one.
      items.push(...pm.rootRows());
    } else if (this.page === 1) {
      row('Download from link','Enter a direct download link.','$download');
      row('Downloads','Load, manage startup or delete downloaded payloads.','$payload-downloads');
      row('Load from storage','Choose an ELF and press X to load it.','$payload-storage');
      row('Autoload config','Edit /data/ps5_autoloader/autoload.txt: load order, adds, removes and timings.','$autoload');
    } else if (this.page === 2) {
      const profile = state.loadouts?.profiles.find(p => p.titleId === state.game?.titleId && p.version === state.game?.version);
      if (profile) {
        row('Loadout: ' + profile.name, 'Review cheats and overlay choices', '$loadout-open', { titleId: profile.titleId, version: profile.version });
        if(!profile.plugins.length)row('Automatic loadout', profile.autostart ? 'On' : 'Off', 'loadout.save',
          { profile: { ...profile, autostart: !profile.autostart }, revision: state.revision }, { on: profile.autostart });
        else row('Saved loadout needs editing', 'This setup contains retired choices. Edit and save it from the phone before applying it.');
      }
      if (state.loadouts?.active) row('Use default overlay', 'Release this session’s loadout overlay; native steps remain', 'loadout.overlay.release');
      const mountService=state.services.find(s=>s.id==='shadowmount');
      if(mountService&&mountService.status==='stopped')row('ShadowMount not running','Load ShadowMount from your own autoloader; the menu adopts it automatically.');
      if(mountService&&['failed','uncertain','blocked'].includes(mountService.status))row('Check ShadowMount',mountService.error?.message || 'Check the running service before retrying.', 'service.reconcile', {id:'shadowmount'});
      row('Refresh library', runtime.libraryState?.error?.message || 'Read installed games and connected game sources', runtime.library ? 'library.refresh' : null);
      // Play and Mount queue behind the resources their operations reserve. An
      // operation whose result is still unknown reserves them, so name it and
      // offer to release it instead of leaving the page stuck at "queued".
      const blocker=(state.jobs||[]).find(j=>['interrupted','uncertain'].includes(j.status));
      if(blocker)row('Blocked by '+blocker.kind,blocker.error?.message||'Its result is unknown, so it still holds this page\'s resources. Check the console, then release it.','$job-release',{id:blocker.id},{confirm:true});
      if(this.gameView){
        const game=runtime.libraryState?.games.find(g=>g.titleId===this.gameView.titleId);
        if(!game)row('Game no longer listed','Refresh the library.');
        else {
          row(game.name,game.titleId+' · '+game.platform);
          row(game.canMount&&!game.mounted?'Mount and play':'Play game','Start this game on the console.',
            runtime.jobs.handlers.has('library.launch')&&!state.safe?'job.enqueue':null,{kind:'library.launch',input:{titleId:game.titleId}},{confirm:true});
          if(game.canMount)row(game.mounted?'Unmount game':'Mount game',game.mounted?'Release the mount after closing the game.':'Make this game source available to the console.',
            game.sourceAvailable&&!state.safe?'job.enqueue':null,{kind:'library.mount',input:{titleId:game.titleId,path:game.path,contentId:game.contentId,mounted:!game.mounted}},{confirm:true});
          const job=state.jobs.filter(j=>['library.launch','library.mount'].includes(j.kind)&&j.args.titleId===game.titleId).at(-1);
          if(job?.status==='interrupted')row('Check last result','Check the previous operation before trying again.','job.reconcile',{id:job.id});
          if(job)row(job.status==='completed'?'Ready':job.status==='failed'?'Game needs attention':'Game progress',job.error?.message||job.result?.message||job.progress?.phase||job.status);
        }
        row('Back to games','','$game-back');
      }else for (const game of runtime.libraryState?.games || []) {
        row(game.name,`${game.titleId} · ${game.platform} · ${game.canMount?(game.mounted?'Mounted':'Needs mounting'):'Installed'}`,'$game-open',{titleId:game.titleId});
      }
    } else if (this.page === 3) {
      items.push(...transferRows(state, this.transferView, state.jobs, runtime.jobs.handlers));
    } else if (this.page === 4) {
      row('Dashboard title ID + version',state.settings.appearance.gameCardDetails?'On · show game ID and installed update beside dashboard titles':'Off · show game ID and installed update beside dashboard titles','settings.update',
        {revision:state.revision,patch:{appearance:{gameCardDetails:!state.settings.appearance.gameCardDetails}}},{on:state.settings.appearance.gameCardDetails});
      for (const [key, label, capability] of [['ip', 'IP address on screen', 'ip-overlay'], ['fps', 'Game FPS', 'fps'],
        ['ram', 'Memory usage', 'resources'], ['cpu', 'CPU usage', 'resources']])
        row(label, feature(capability) ? (state.settings.overlay[key] ? 'On' : 'Off') : 'Renderer is not ready',
          feature(capability) ? 'settings.update' : null,
          { revision: state.revision, patch: { overlay: { [key]: !state.settings.overlay[key] } } }, { on: state.settings.overlay[key] });
      row('Reduced motion', state.settings.appearance.reducedMotion ? 'On' : 'Off', 'settings.update',
        { revision: state.revision, patch: { appearance: { reducedMotion: !state.settings.appearance.reducedMotion } } }, { on: state.settings.appearance.reducedMotion });
      row('ELF loader boot animation', describeBootAnimation(!!state.settings.bootAnimation?.enabled, runtime.bootAnimation?.count ?? 0), 'settings.update',
        { revision: state.revision, patch: { bootAnimation: { enabled: !state.settings.bootAnimation?.enabled } } }, { on: !!state.settings.bootAnimation?.enabled });
      row('MP4 folder', BOOTANIM_FOLDER + ' · drop one or more MP4 (H.264/AAC) videos here; more than one is chosen at random');
      row('Preview animation', runtime.bootAnimation?.playing ? 'Playing now — pressing it again is refused until it finishes' : 'Play one now, chosen at random from the folder',
        runtime.bootAnimation?.preview ? '$bootanim-preview' : null);
      if(state.recovery && (state.safe || state.recovery.running))row('Recover services','Review startup before continuing.', '$startup-review');
      for(const service of state.services.filter(s=>['ftp'].includes(s.id)))row('FTP server', service.status==='ready'?'Running':service.error?.message||'Start this service', service.status==='stopped'?'service.start':null,{id:service.id});
      row('Session history','Review service changes and available undo actions.','$history');
      row('Adaptive notices', state.settings.overlay.adaptive ? 'Brief session updates are on' : 'Off; results remain in Activity', 'settings.update',
        { revision: state.revision, patch: { overlay: { adaptive: !state.settings.overlay.adaptive } } }, { on: state.settings.overlay.adaptive });
    } else if (this.cheatPack) {
      const pack = this.cheatPack, matches = state.game?.titleId === pack.titleId && state.game?.version === pack.version;
      row(pack.name, `${pack.titleId} · ${pack.version}`);
      for (const cheat of pack.cheats) {
        const job = state.jobs.filter(j => j.kind === 'cheat.set' && j.args.sha256 === pack.sha256 && j.args.cheatId === cheat.id && j.args.generation === state.game?.generation).at(-1);
        const active = job && !['completed', 'failed', 'cancelled'].includes(job.status);
        const on = job?.status === 'completed' && job.result?.phase === 'applied' && job.args.enabled;
        row(cheat.name, active ? job.error?.message || job.status : !matches ? 'Open the exact target game and version' : !cheat.supported ? 'Advanced section addressing is not supported' : on ? (cheat.exactUndo === false ? 'On. Select to apply the file’s Off bytes; extra On bytes may remain until the game closes.' : 'Last applied: On. Select to restore original bytes.') : 'Select to enable; expected bytes are checked first.',
          matches && !active && cheat.supported && feature('cheats') && !state.safe ? 'job.enqueue' : null,
          { kind: 'cheat.set', input: { sha256: pack.sha256, cheatId: cheat.id, enabled: !on } }, { on: Boolean(on) });
      }
      row('Other cheat packs', 'Return to the cheat library', '$cheat-back');
    } else {
      if(runtime.jobs.handlers.has('cheat.sync'))row('Download latest cheats','Fetch cheats for installed games from etaHEN and GoldHEN. Files are matched to their exact game version.','job.enqueue',{kind:'cheat.sync',input:{}});
      const sync=state.jobs.filter(j=>j.kind==='cheat.sync').at(-1);
      if(sync)row(sync.status==='completed'?'Cheats updated':sync.status==='failed'?'Cheat download failed':'Updating cheats',this.nativeSettings&&syncingCheats?'Downloading in the background. Your current list stays available.':sync.error?.message||sync.result?.message||sync.progress?.phase||sync.status);
      row('Import cheat file', 'Choose JSON, SHN/XML or MC4 from USB or console storage. No game needs to be running.', '$browse', {kind:'cheat'});
      for (const pack of cheatCatalog.filter(p=>!this.cheatTitle||p.titleId===this.cheatTitle)) row(pack.name, `${pack.titleId} · ${pack.version} · ${pack.count} cheats`, '$cheat-open', { sha256: pack.sha256 });
      if(this.cheatTitle){if(!cheatCatalog.some(p=>p.titleId===this.cheatTitle))row('No cheats found for '+this.cheatTitle,'Download the latest cheats or import a matching file.');row('Show all games','Browse the complete cheat library.','$cheat-all');}
      if (!items.length) row('No cheat files imported', 'Choose Import cheat file to add cheats.');
    }
    const grid=this.page===0&&!this.managerView&&!this.payloadDownloads&&!this.textInput&&!this.downloadView&&!this.startupPayloadView&&!this.confirmation&&!this.fileView&&!this.importJob&&!this.packageView&&!this.saveView&&!this.historyView&&!this.startupReview;
    const browser=this.managerView&&!this.textInput&&!this.confirmation?fileManagerFrame(this.managerView,state.jobs):null;
    // ProsperoMenu ships one 32-row window of the list around the focused item.
    // The deck owns focus and scroll; a 'window' action moves the window. Long lists scroll instead of paging.
    this.offset = Math.min(this.offset, Math.max(0, Math.min(65535, items.length) - DECK_WIRE.rows));
    const selected = items.slice(this.offset, this.offset + DECK_WIRE.rows);
    const data = { ready: true, grid, browser, keyboard:!!this.textInput, reducedMotion: state.settings.appearance.reducedMotion, accent: state.settings.appearance.accent, ipVisible: state.settings.overlay.ip, ip: state.identity.ip,
      windowStart: this.offset, total: Math.min(65535, items.length),
      focus: pmIdle&&!pmView&&pm.rootFocus!=null&&pm.rootFocus<items.length?pm.rootFocus:0xFFFF,
      theme: pm.theme(),
      logoPath: pm.logoPath(),
      videoSerial: pm.videoSerial ? pm.videoSerial() : 0,
      canBack: Boolean(pmView || this.managerView || this.payloadDownloads || this.textInput || this.downloadView || this.startupPayloadView || this.fileView || this.importJob || this.page || this.confirmation  || this.packageView || this.loadoutPreview || this.saveView || this.historyView || this.startupReview || this.autoloadView || this.dumperView || this.elfView),
      page: this.page, title: pmView ? 'ProsperoMenu / ' + (pmView.title || 'Overlay') : pmIdle ? 'ProsperoMenu' : `ProsperoMenu / ${this.confirmation ? 'Confirm action' : this.textInput ? (this.textInput.field==='url'?'Enter download link':this.textInput.operation==='mkdir'?'New folder':'Enter filename') : this.managerView ? 'File browser' : this.payloadDownloads ? (this.payloadDownloads.load?'Load payload':'Downloads') : this.downloadView && !this.confirmation ? 'Download payload' : this.confirmation ? 'Confirm action' : this.startupPayloadView ? 'Startup payloads' : this.autoloadView ? 'Startup order' : this.elfView ? 'Elf-Updater' : this.dumperView ? 'App Dumper' : this.importJob ? 'Preparing file' : this.fileView ? 'Choose a file' : this.packageView ? 'Packages' : this.historyView ? 'Session history' : this.saveView ? 'Saved games' : (pages[this.page]||'Cheats')}`,
      subtitle: this.notice || pm.notice || (pmView ? pmView.subtitle : null) || browser?.selection || (this.fileView?.source || (state.safe ? 'Recovery needs attention' : state.game ? state.game.name + ' · ' + state.game.titleId : 'Firmware ' + (state.identity.firmware || 'development'))),
      footer: pmView?.footer ? pmView.footer : browser ? 'D-pad Browse   L1/R1 Pane   X Open/select   Circle Up   Bottom bar: actions' : this.textInput ? 'D-pad Move   X Type   L1/R1 abc/ABC/Symbols   Circle Cancel' : 'X  Select     Circle  Back     L3 + R3  Close', rows: selected };
    const context=this.nativeSettings?JSON.stringify([this.page,this.offset,this.confirmation,this.cheatPack?.sha256,this.cheatTitle,state.game?.generation]):null;
    const signature = JSON.stringify(this.nativeSettings?[data,context]:data);
    if (signature !== this.signature) {
      this.signature = signature; this.sequence = (this.sequence + 1) >>> 0 || 1;
      // Native settings keeps its row objects across background updates. A token
      // survives only if the entire row AND its action context are unchanged.
      const previous=this.nativeSettings&&context===this.nativeRowContext ? [...(this.current?.rows||[])] : [];
      this.nativeRowContext=context;
      this.current = { ...data, sequence: this.sequence, rows: selected.map(item => {
        const index=previous.findIndex(({token,...old})=>JSON.stringify(old)===JSON.stringify(item));
        const token=index>=0?previous.splice(index,1)[0].token:runtime.id();
        return {...item,token};
      }) };
    }
    const metrics = state.game ? telemetryText(state) : '';
    if (metrics !== this.metrics) { this.metrics = metrics;this.metricsSequence = (this.metricsSequence + 1) >>> 0 || 1; }
    // Telemetry never changes action tokens or the selected menu's sequence.
    return { ...this.current, metrics, metricsSequence: this.metricsSequence, notices: state.notices, input: runtime.gameInputRelay?.frame(),dashboard:this.dashboard.frame() };
  }
  action(input) {
    invariant(input && /^[a-f0-9]{32}$/.test(input.request || ''), 'DECK_REQUEST', 'Invalid controller request.', { status: 400 });
    const signature = JSON.stringify([input.kind, input.sequence, input.row]);
    const previous = this.requests.get(input.request);
    if (previous) {
      invariant(signature === previous.signature, 'DECK_REQUEST_REUSED', 'Controller request was reused for another action.');
      return previous.result;
    }
    const run = async () => {
      // A window move is positional, not a row action: a background update that
      // bumps the sequence must not strand the deck at a window edge. The deck
      // re-sends the request anchored to its focus if anything races.
      if (input.kind === 'window') {
        const current = this.frame();
        this.offset = Math.min(Math.max(0, input.row | 0), Math.max(0, Math.min(65535, current.total) - DECK_WIRE.rows));
        return this.frame();
      }
      // Rebuild before checking the displayed sequence so a phone update cannot
      // turn an old confirmation into a different or opposite action.
      const frame = this.frame();
      invariant(Number.isInteger(input.sequence) && input.sequence === frame.sequence, 'DECK_STALE', 'The deck changed. Select the action again.');
      this.prospero.notice='';
      const browserRow=frame.browser&&Number.isInteger(input.row)?input.row%32:input.row;
      if(frame.browser&&Number.isInteger(input.row)&&input.row>=24)this.managerView.active=input.row>=32?1:0;
      if(this.textInput){
        const effect=editDownload(this.textInput,input.kind,input.row);
        if(effect==='done'&&this.textInput.operation){
          const {operation,value,target,source}=this.textInput;
          this.confirmation={label:operation==='mkdir'?'Create folder '+value:'Rename to '+value,detail:target,command:'job.enqueue',args:operation==='mkdir'?{kind:'file.mkdir',input:{destination:target,name:value}}:{kind:'file.transfer',input:{...source,destination:target,destinationName:value,move:true}}};this.textInput=null;
        }else if(effect==='done'){const {field,value}=this.textInput;this.downloadView[field]=value;
          if(field==='url'&&!this.downloadView.name)this.downloadView.name=suggestedName(value);this.textInput=null;}
        else if(effect==='cancel')this.textInput=null;
        this.notice='';return this.frame();
      }
      if ((input.kind === 'next' || input.kind === 'previous') && frame.rows[input.row]?.adjust) {
        // Left/Right on a slider or picker row (PM_ROW_ADJUST).
        const row=frame.rows[input.row];
        if(row.command?.startsWith('$pm-'))this.prospero.adjust(row,input.kind);
        else if(this.autoloadView&&(row.command==='$autoload-entry'||row.command==='$autoload-wait')){
          // Left/Right on a wait: the list row changes that wait, the entry
          // page changes the open entry's wait.
          const delta=input.kind==='next'?1:-1;
          await autoloadEdit(this.runtime,this.autoloadView,row.command==='$autoload-entry'?{kind:'wait-step',index:row.args.index,delta}:{kind:'wait-step',delta});
        }
        return this.frame();
      }
      if (input.kind === 'next' || input.kind === 'previous') {
        this.page = (this.page + (input.kind === 'next' ? 1 : pages.length - 1)) % pages.length;
        this.offset = 0; this.confirmation = null; this.startupPayloadView=null;this.transferView=null; this.managerView=null;this.payloadDownloads=null;this.gameView=null; this.downloadView=null; this.fileView=null;this.importJob=null;this.packageView=null;this.loadoutPreview = null; this.saveView = null;this.historyView = null;this.startupReview = null; this.notice = '';if(this.page===5)await this.openCheats(); return this.frame();
      }
      if (input.kind === 'back') {
        if(this.pmIdle()&&this.prospero.active()){await this.prospero.back();this.offset=0;this.notice='';return this.frame();}
        if (this.confirmation && (this.managerView || this.payloadDownloads || this.gameView || this.downloadView || this.transferView || this.startupPayloadView || this.loadoutPreview || this.saveView || this.historyView || this.startupReview||this.packageView||this.fileView||this.importJob||this.autoloadView||this.elfView||this.dumperView)) this.confirmation = null;
        else if(this.managerView){await this.managerBack(browserRow);}
        else if(this.payloadDownloads)this.payloadDownloads=this.payloadDownloads.id?{}:null;
        else if(this.gameView)this.gameView=null;
        else if(this.downloadView){if(this.downloadView.job)delete this.downloadView.job;else this.downloadView=null;}
        else if(this.startupPayloadView)this.startupPayloadView=this.startupPayloadView.id?{}:null;
        else if(this.transferView?.id)this.transferView={section:this.transferView.section,...(this.transferView.parent?{id:this.transferView.parent}:{})};
        else if(this.transferView?.section)this.transferView={};
        else if(this.importJob)this.importJob=null;
        else if(this.fileView){if(this.fileView.source)this.fileView=await this.runtime.execute('files.browse',{kind:this.fileView.kind,source:this.fileView.parent});else this.fileView=null;}
        else if (this.packageView) this.packageView = this.packageView.id ? (this.runtime.packageItems?.find(p=>p.id===this.packageView.id)?.external?{}:{downloads:true}) : (this.packageView.downloads||this.packageView.receiver||this.packageView.activity||this.packageView.workflow) ? {} : null;
        else if (this.startupReview) { this.startupReview = null;this.packageView=this.startupReturnPackage||null;this.startupReturnPackage=null; }
        else if (this.historyView) { if (this.historyView.id) this.historyView = {};else this.historyView = null; }
        else if (this.saveView) { if(this.saveView.job)this.saveView={offset:0,selected:null};else if(this.saveView.usb)this.saveView.usb=false;else if (this.saveView.selected) this.saveView.selected = null;else this.saveView = null; }
        else if (this.autoloadView) {
          // Circle walks the page stack: entry -> list -> chooser -> out.
          if (this.autoloadView.kind === 'entry' || this.autoloadView.kind === 'add') this.autoloadView.kind = 'list';
          else if (this.autoloadView.kind === 'list' && this.autoloadView.sources) this.autoloadView = { kind: 'choose', sources: this.autoloadView.sources, notes: this.autoloadView.notes };
          else this.autoloadView = null;
          // Back on Elf-Updater: show the startup order as it is now.
          if (!this.autoloadView && this.elfView) this.elfView.startup = await readStartup(this.runtime).catch(() => this.elfView.startup);
        }
        else if (this.elfView) {
          if (this.elfView.kind === 'versions') this.elfView = { ...this.elfView, kind: 'source' };
          else if (this.elfView.kind === 'source') this.elfView = { ...this.elfView, kind: 'main', id: null };
          else this.elfView = null;
        }
        else if (this.dumperView) {
          // Circle leaves the picker or the overlay itself.
          if (this.dumperView.kind === 'titles' || this.dumperView.kind === 'targets') this.dumperView = { ...this.dumperView, kind: 'main' };
          else this.dumperView = null;
        }
        else { this.confirmation = null;if (this.loadoutPreview) this.loadoutPreview = null;else if (this.page === 5 && this.cheatPack) this.cheatPack = null;else this.page = 0; }
        this.offset = 0;this.notice = '';return this.frame();
      }
      const actionRows=frame.browser?[...frame.browser.panes.flatMap(p=>p.entries),...frame.rows]:frame.rows;
      invariant(input.kind === 'select' && Number.isInteger(input.row) && input.row >= 0 && browserRow < actionRows.length && (!frame.browser||input.row<64),
        'DECK_ACTION', 'Invalid deck action.', { status: 400 });
      let selected = actionRows[browserRow];
      invariant(selected.enabled, 'DECK_DISABLED', 'This action is not available.');
      // Information rows are drawn like normal rows (not in the colour that
      // explains why an action is unavailable); X does nothing on them.
      if (selected.command === '$info') return this.frame();
      // ProsperoMenu rows. A builtin overlay hands back the ProsperoMenu command for
      // its page; any other ProsperoMenu command started from a ProsperoMenu page
      // (console settings, file browser) returns there afterwards.
      if (selected.command?.startsWith('$pm-') && !selected.confirm) {
        const next = await this.prospero.select(selected);this.offset=0;this.notice='';
        if (!next) return this.frame();
        if (next.fromOverlay) this.prospero.returnFocusId = next.fromOverlay;
        selected = { ...selected, command: next.command, args: next.args };
      } else if (this.pmIdle() && this.prospero.active() && selected.command !== '$confirm' && selected.command !== '$cancel' && !selected.confirm) {
        this.prospero.suspended = this.prospero.location;this.prospero.location = null;
      }
      // A confirmed deck command ($...) runs through the same handlers below.
      if (selected.command === '$confirm' && this.confirmation?.command?.startsWith('$') && !this.confirmation.command.startsWith('$pm-')) {
        selected = { ...this.confirmation, confirm: false };this.confirmation = null;
      }
      // ...and one that asks first stops here until it is confirmed.
      else if (selected.confirm && selected.command?.startsWith('$') && !selected.command.startsWith('$pm-')) { this.confirmation = selected;return this.frame(); }
      if(selected.command==='$page'){this.page=selected.args.page;this.offset=0;if(this.page===2)this.refreshLibrary();if(this.page===5)await this.openCheats();}
      else if(selected.command==='$game-open'){this.gameView=selected.args;this.offset=0;}
      else if(selected.command==='$game-back'){this.gameView=null;this.offset=0;}
      else if(selected.command==='$transfer-open'){this.transferView={section:this.transferView?.section,id:selected.args.id,parent:selected.args.parent};this.offset=0;}
      else if(selected.command==='$transfer-back'){this.transferView={section:this.transferView?.section,...(this.transferView?.parent?{id:this.transferView.parent}:{})};this.offset=0;}
      else if(selected.command==='$autoload'){this.autoloadView=await startAutoload(this.runtime,{notes:updaterNotes(this.runtime)});this.offset=0;this.notice='';}
      else if(selected.command==='$autoload-refresh'){this.autoloadView=await refreshSource(this.runtime,this.autoloadView);this.offset=0;}
      else if(selected.command==='$autoload-toggle'){await autoloadEdit(this.runtime,this.autoloadView,{kind:'toggle'});this.offset=0;}
      else if(selected.command==='$elfup'){this.elfView=await openElfUpdater(this.runtime);this.offset=0;this.notice='';}
      else if(selected.command==='$elfup-refresh'){this.elfView=await openElfUpdater(this.runtime,{kind:this.elfView?.kind||'main',id:this.elfView?.id});this.offset=0;this.notice='Refreshed';}
      else if(selected.command==='$elfup-source'){this.elfView={...this.elfView,kind:'source',id:selected.args.id};this.offset=0;}
      else if(selected.command==='$elfup-versions'){this.elfView={...this.elfView,kind:'versions',id:selected.args.id};this.offset=0;}
      else if(selected.command==='$elfup-back'){this.elfView={...this.elfView,kind:this.elfView.kind==='versions'?'source':'main'};this.offset=0;}
      else if(selected.command==='$elfup-prerelease'){await this.runtime.elfUpdater.setPrereleases(!this.runtime.elfUpdater.status().includePrereleases);this.offset=0;}
      else if(selected.command==='$elfup-autoload'||selected.command==='$elfup-startup'){
        const view=await openInternal(this.runtime,updaterNotes(this.runtime));
        const item=selected.args?.id?this.runtime.elfUpdater.status().items.find(i=>i.id===selected.args.id):null;
        if(item){let index=findPayload(view,item.file);
          if(index<0){await autoloadEdit(this.runtime,view,{kind:'add-file',name:item.file});this.notice=item.file+' added to the startup order';}
          else {view.kind='entry';view.index=index;}}
        this.autoloadView=view;this.offset=0;}
      else if(selected.command==='$autoload-source'){const s=this.autoloadView.sources.find(x=>x.id===selected.args.id);if(s){const opened=openSource(s);opened.sources=this.autoloadView.sources;this.autoloadView=opened;}this.offset=0;}
      else if(selected.command==='$autoload-list'){this.autoloadView.kind='list';this.offset=0;}
      else if(selected.command==='$autoload-add'){this.autoloadView.kind='add';this.offset=0;}
      else if(selected.command==='$autoload-entry'){this.autoloadView.kind='entry';this.autoloadView.index=selected.args.index;this.offset=0;}
      else if(selected.command==='$autoload-move'){await autoloadEdit(this.runtime,this.autoloadView,{kind:'move',delta:selected.args.delta,to:selected.args.to});this.autoloadView.kind='entry';this.offset=0;}
      else if(selected.command==='$autoload-remove'){await autoloadEdit(this.runtime,this.autoloadView,{kind:'remove'});this.offset=0;}
      else if(selected.command==='$autoload-wait'){await autoloadEdit(this.runtime,this.autoloadView,{kind:'wait'});this.autoloadView.kind='entry';this.offset=0;}
      else if(selected.command==='$autoload-add-file'){await autoloadEdit(this.runtime,this.autoloadView,{kind:'add-file',name:selected.args.name});this.offset=0;}
      else if(selected.command==='$autoload-insert-wait'){await autoloadEdit(this.runtime,this.autoloadView,{kind:'insert-wait'});this.offset=0;}
      else if(selected.command==='$dumper'){this.dumperView=await loadDumper(this.runtime);this.offset=0;this.notice='';}
      else if(selected.command==='$dumper-refresh'){this.dumperView=await loadDumper(this.runtime,{...this.dumperView,kind:'main'});this.offset=0;}
      else if(selected.command==='$dumper-titles'){this.dumperView=await loadDumper(this.runtime,{...this.dumperView,kind:'titles'});this.offset=0;}
      else if(selected.command==='$dumper-targets'){this.dumperView=await loadDumper(this.runtime,{...this.dumperView,kind:'targets'});this.offset=0;}
      else if(selected.command==='$dumper-pick'){this.dumperView={...this.dumperView,kind:'main',title:(this.dumperView.titles||[]).find(a=>a.titleId===selected.args.titleId)||null};this.offset=0;}
      else if(selected.command==='$dumper-target'){this.dumperView={...this.dumperView,kind:'main',target:(this.dumperView.targets||[]).find(t=>t.mount===selected.args.mount)||null};this.offset=0;}
      else if(selected.command==='$dumper-back'){this.dumperView={...this.dumperView,kind:'main'};this.offset=0;}
      else if(selected.command==='$dumper-start'){this.dumperView=await dumperStart(this.runtime,this.dumperView);this.offset=0;this.notice=this.dumperView.notice;}
      else if(selected.command==='$dumper-stop'){this.dumperView=await dumperStop(this.runtime,this.dumperView);this.offset=0;}
      else if(selected.command==='$job-release'){await this.runtime.execute('job.release',{id:selected.args.id});this.notice='Reservation released';this.offset=0;}
      else if(selected.command==='$transfer-section'){this.transferView={section:selected.args.section};this.offset=0;}
      else if(selected.command==='$file-manager'){this.managerView=this.suspendedManager||await openFileManager(this.runtime);this.suspendedManager=null;await refreshFileManager(this.runtime,this.managerView);this.offset=0;}
      else if(selected.command==='$manager-folder'){await this.managerFolder(selected.args.side,selected.args.source);}
      else if(selected.command==='$manager-entry'){const {side,item}=selected.args;this.managerView.active=side;if(item.directory&&(item.ino==null||this.managerView.selected?.source===item.source&&this.managerView.selected?.pane===side))await this.managerFolder(side,item.source);else this.managerView.selected={...item,pane:side};}
      else if(selected.command==='$manager-page'){const {side,offset}=selected.args;this.managerView.active=side;this.managerView.panes[side].offset=offset;}
      else if(selected.command==='$manager-refresh'){await refreshFileManager(this.runtime,this.managerView);}
      else if(selected.command==='$manager-exit'){this.managerView=null;this.offset=0;}
      else if(selected.command==='$manager-transfers'){this.suspendedManager=this.managerView;this.managerView=null;this.transferView={section:'active'};this.offset=0;}
      else if(selected.command==='$manager-name'){const v=this.managerView,operation=selected.args.operation,source=v.selected,target=operation==='mkdir'?v.panes[v.active].listing.source:source.source.slice(0,source.source.lastIndexOf('/'));
        this.textInput={field:'filename',operation,source,target,page:0,value:operation==='rename'?source.name:''};this.notice='';this.offset=0;}
      else if(selected.command==='$payload-downloads'){this.payloadDownloads={};this.downloadView=null;this.offset=0;}
      else if(selected.command==='$payload-file'){this.payloadDownloads=selected.args;this.downloadView=null;this.offset=0;}
      else if(selected.command==='$payload-downloads-exit'){this.payloadDownloads=null;this.offset=0;}
      else if(selected.command==='$package-downloads'){this.packageView={downloads:true};this.offset=0;}
      else if(selected.command==='$transfer-filter'){this.transferView={all:!this.transferView?.all};this.offset=0;}
      else if(selected.command==='$startup-payloads'){this.startupPayloadView={};this.offset=0;}
      else if(selected.command==='$startup-payload-open'){this.startupPayloadView=selected.args;this.offset=0;}
      else if(selected.command==='$download'){this.downloadView={};this.offset=0;}
      else if(selected.command==='$download-edit'){const field=selected.args.field;this.textInput={field,page:0,value:this.downloadView[field]||(field==='url'?'https://':'payload.elf')};this.notice='';this.offset=0;}
      else if(selected.command==='$download-exit'){this.downloadView=null;this.offset=0;}
      else if(selected.command==='$browse'){this.fileView=await runtimeBrowse(this.runtime,selected.args);this.offset=0;}
      else if(selected.command==='$package-storage'||selected.command==='$payload-storage'){
        const kind=selected.command==='$package-storage'?'package':'payload';
        const roots=await runtimeBrowse(this.runtime,{kind}),usb=roots.entries.filter(e=>e.usb);
        this.fileView=usb.length===1?await runtimeBrowse(this.runtime,{kind,source:usb[0].source}):roots;this.offset=0;
      }
      else if(selected.command==='$files-exit'){this.fileView=null;this.offset=0;}
      else if(selected.command==='$import-back'){this.importJob=null;this.offset=0;}
      else if(selected.command==='$import-open'){this.importJob=null;this.fileView=null;this.offset=0;if(selected.args.kind==='package')this.packageView={id:selected.args.id};else if(selected.args.kind==='cheat'){this.page=5;this.cheatPack=await this.runtime.execute('cheat.inspect',{sha256:selected.args.id});}else {this.page=1;this.payloadDownloads={id:selected.args.id};}}
      else if (selected.command === '$packages') { this.packageView = {}; this.offset = 0; }
      else if (selected.command === '$package-open') { this.packageView = selected.args; this.offset = 0; }
      else if (selected.command === '$package-exit') { this.packageView = null; this.offset = 0; }
      else if (selected.command === '$notice-dismiss') {
        const active = this.runtime.snapshot().notices.current;
        if (active) await this.runtime.execute('notice.dismiss', { id: active.id });
      }
      else if (selected.command === '$history') { this.historyView = {};this.offset = 0; }
      else if (selected.command === '$bootanim-preview') {
        const result = await this.runtime.execute('bootanim.preview', {});
        this.notice = result.started ? 'Playing ' + result.clip : 'Preview skipped: ' + result.reason;
        this.offset = 0;
      }
      else if (selected.command === '$startup-review') {
        const plan = await this.runtime.execute('startup.preview', {});
        if(this.packageView){this.startupReturnPackage=this.packageView;this.packageView=null;}
        this.startupReview=plan;this.offset=0;
      }
      else if (selected.command === '$startup-back') { this.startupReview = null;this.packageView=this.startupReturnPackage||null;this.startupReturnPackage=null;this.offset = 0; }
      else if (selected.command === '$history-open') { this.historyView = selected.args;this.offset = 0; }
      else if (selected.command === '$history-back') { this.historyView = {};this.offset = 0; }
      else if (selected.command === '$saves') { this.saveView = { offset: 0, selected: null };this.offset = 0;await this.refreshSaves(); }
      else if(selected.command==='$save-usb'){this.saveView.usb=true;this.offset=0;}
      else if(selected.command==='$save-destinations'){this.saveView.usb=false;this.offset=0;}
      else if(selected.command==='$save-list'){this.saveView={offset:0,selected:null};this.offset=0;}
      else if (selected.command === '$save-open') { this.saveView.selected = selected.args;this.offset = 0; }
      else if (selected.command === '$save-page') { this.saveView.offset = selected.args.offset;this.offset = 0; }
      else if (selected.command === '$save-back') { this.saveView.selected = null;this.offset = 0; }
      else if (selected.command === '$loadout-open') { this.loadoutPreview = await this.runtime.execute('loadout.preview', selected.args);this.offset = 0; }
      else if (selected.command === '$loadout-back') { this.loadoutPreview = null;this.offset = 0; }
      else if (selected.command === '$cheat-open') { this.cheatPack = await this.runtime.execute('cheat.inspect', selected.args); this.offset = 0; }
      else if (selected.command === '$cheat-back') { this.cheatPack = null; this.offset = 0; }
      else if (selected.command === '$cheat-all') {this.cheatTitle=null;this.offset=0;this.notice='';}
      else if (selected.command === '$cancel') this.confirmation = null;
      else if (selected.command === '$confirm') {
        const pending = this.confirmation; this.confirmation = null;
        if (pending.command?.startsWith('$pm-')) await this.prospero.select(pending);
        else await this.dispatch(pending, input.request);
      } else if (selected.confirm) this.confirmation = selected;
      else await this.dispatch(selected, input.request);
      return this.frame();
    };
    const result = this.tail.then(run).catch(error => { this.notice = error.message; throw error; });
    this.tail = result.catch(() => {});
    this.requests.set(input.request, { signature, result });
    if (this.requests.size > 128) this.requests.delete(this.requests.keys().next().value);
    return result;
  }
  async managerFolder(side,source){const v=this.managerView;v.active=side;v.panes[side]={listing:await this.runtime.execute('files.list',{source}),offset:0};this.offset=0;}
  async managerBack(row){
    const v=this.managerView,side=Number.isInteger(row)&&row<24?Math.floor(row/12):v.active;
    if(v.panes[side].listing.source)await this.managerFolder(side,v.panes[side].listing.parent);else this.managerView=null;
  }
  refreshCheats(){
    if(this.cheatsRequested||!this.runtime.jobs.handlers.has('cheat.sync')||this.runtime.graph.safe)return;
    this.cheatsRequested=true;
    this.runtime.execute('job.enqueue',{kind:'cheat.sync',input:{},key:this.runtime.id()}).catch(error=>{this.notice=error.message;this.cheatsRequested=false;});
  }
  refreshLibrary(automatic = false) {
    if (this.libraryPending || !this.runtime.library) return this.libraryPending;
    if(!automatic)this.notice = 'Finding games...';
    // Automatic and manual refresh deliberately use the identical command.
    // Keep selection/navigation and the current action's notice unchanged.
    this.libraryPending = this.runtime.execute('library.refresh', {}).then(() => { if(!automatic)this.notice = 'Game list updated.'; }, error => { if(!automatic)this.notice = error.message; }).finally(() => { this.libraryPending = null; });
    return this.libraryPending;
  }
  async refreshSaves() {
    if (!this.runtime.saves || this.runtime.graph.safe) return;
    const current = this.runtime.jobs.snapshot().find(j => j.kind === 'save.refresh' && !['completed','failed','cancelled'].includes(j.status));
    if (!current && !this.runtime.saves.list({}).updatedAt) await this.runtime.execute('job.enqueue', { kind:'save.refresh', input:{}, key:this.runtime.id() });
  }
  async dispatch(row, key) {
    try {
      if (row.packageReview) {
        const current = packageDeckRows(this.runtime.snapshot(), { id: row.packageReview }, this.runtime.jobs.snapshot());
        invariant(current.some(item => item.enabled && item.command === row.command && JSON.stringify(item.args) === JSON.stringify(row.args)),
          'PACKAGE_REVIEW_CHANGED', 'The package or its operation changed. Review it again before installing.');
      }
      this.notice = 'Working…';
      if (row.command === 'library.refresh') { this.refreshLibrary();return; }
      const result=await this.runtime.execute(row.command, row.command === 'job.enqueue' ? { ...row.args, key } : row.args || {});
      if(row.command==='job.enqueue'&&row.args.kind==='payload.download'&&this.downloadView){this.downloadView.job=result.id;this.offset=0;}
      if(row.command==='job.enqueue'&&row.args.kind==='save.export'&&this.saveView){this.saveView.job=result.id;this.offset=0;}
      if(row.command==='job.enqueue'&&['file.transfer','file.mkdir','file.delete'].includes(row.args.kind)&&this.managerView){this.managerView.job=result.id;this.managerView.message='';this.offset=0;}
      if(row.command==='download.delete'){if(row.args.kind==='payload')this.payloadDownloads={};else this.packageView=row.label==='Remove from list'?{}:{downloads:true};this.offset=0;}
      if(row.fileImport){this.importJob={id:result.id,kind:row.fileImport};this.offset=0;}
      if(row.packageWorkflow){this.fileView=null;this.importJob=null;this.packageView={workflow:result.id};this.offset=0;}
      if(row.payloadWorkflow){this.fileView=null;this.importJob=null;this.payloadDownloads={load:continuePayloadLoad(this.runtime,result)};this.offset=0;}
      if(row.payloadResult){this.downloadView=null;this.payloadDownloads={load:{file:row.payloadFile,trackJob:row.command==='job.enqueue',job:result?.id,phase:row.command==='service.start'?'ready':'loading',message:row.command==='service.start'?'Service started and ready.':null}};this.offset=0;}
      this.notice = row.payloadWorkflow?'Preparing payload. Loading will follow automatically.':row.payloadResult?'Loading status is shown here.':row.command === 'job.enqueue' ? (row.packageWorkflow||row.args.kind==='package.install'?'Installation started. Progress is shown here.':'Accepted. Follow progress in Transfers.') : (result && result.message ? result.message : 'Updated.');
    } catch (error) { this.notice = error.message; throw error; }
  }
}

async function runtimeBrowse(runtime,args){return runtime.execute('files.browse',args);}
