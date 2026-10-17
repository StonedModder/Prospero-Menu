// SPDX-License-Identifier: GPL-3.0-or-later
const names = { 'package.receive': 'Receive and install package', 'package.download': 'Download package', 'cheat.sync': 'Download cheats', 'file.import': 'Copy file', 'payload.download': 'Download payload', 'payload.dispatch': 'Load payload', 'payload.startup': 'Startup payload',
  'package.verify': 'Check package', 'package.install': 'Install package', 'save.refresh': 'Find saved games',
  'save.export': 'Back up save', 'component.start': 'Start service', 'deck.start': 'Start menu',
  'cheat.set': 'Change cheat', 'plugin.start': 'Start plugin', 'plugin.stop': 'Stop plugin', 'library.mount': 'Mount game' };
const live = status => ['queued', 'running', 'cancelling', 'uncertain', 'held', 'interrupted'].includes(status);
const labels = { queued: 'Waiting', running: 'In progress', cancelling: 'Cancelling', completed: 'Finished',
  failed: 'Failed', cancelled: 'Cancelled', held: 'Paused', uncertain: 'Checking needed', interrupted: 'Checking needed' };
const fileKinds = new Set(['file.mkdir','file.delete','file.transfer','package.receive','package.download','file.import','payload.download','package.verify','package.install','save.export','library.storage','cheat.sync']);
export const transferName = job => job.args?.name || (job.kind==='save.export' ? 'Save backup: '+(job.args?.titleId||'Saved game') : names[job.kind] || job.kind.replace(/[._-]+/g, ' ').replace(/^./, x => x.toUpperCase()));
function packageSteps(job,jobs){
  if(job.kind!=='package.receive')return [];
  return jobs.filter(child=>child.id===job.checkpoint?.stage||child.id===job.checkpoint?.install||child.id===job.result?.install||
    child.key==='dpi_stage_'+job.id||child.key==='dpi_install_'+job.id);
}
export function transferDetail(job, checking = false) {
  if (checking) return 'Checking export · Please wait';
  const p = job.progress;
  const percent = p?.total > 0 && !p.indeterminate ? Math.min(100, Math.floor(p.completed * 100 / p.total)) + '% · ' : '';
  return `${labels[job.status] || job.status} · ${job.status === 'running' ? percent : ''}${job.error?.message || job.result?.message || p?.phase || 'Waiting for progress'}`;
}
export function transferRows(state, view = {}, jobs = state.jobs, handlers = new Map()) {
  view ||= {};
  const rows = [], row = (label, detail, command = null, args = {}, extra = {}) => rows.push({ label, detail, command, args, enabled: Boolean(command), ...extra });
  if(!view.id&&!view.section){
    row('File browser','Copy files and folders, or move files, between console storage and USB.','$file-manager',{});
    row('Active transfers','Progress, paused transfers and anything needing attention.','$transfer-section',{section:'active'});
    row('Completed transfers','Finished downloads, installs and file copies.','$transfer-section',{section:'completed'});
    row('FTP connection',`Connect to ${state.identity?.ip||'PS5-IP'}:${state.services?.find(x=>x.id==='ftp')?.evidence?.port||2122}. FTP opens /data/ProsperoMenu/share. Use File browser for console storage and USB.`);
    return rows;
  }
  if (view.id) {
    const job = jobs.find(j => j.id === view.id);
    if (!job) row('Transfer no longer listed', 'Open the history to find older results.');
    else {
      const checking = state.saves?.recovering?.includes(job.id);
      row(transferName(job), transferDetail(job, checking));
      if (job.error?.action) row('What to do next', job.error.action);
      if (job.result?.output) row('Saved file', job.result.output);
      if (job.result?.ftpPath) row('Copy to your computer', `FTP ${state.identity.ip}:2122 → ${job.result.ftpPath}`);
      for(const child of packageSteps(job,jobs))row(names[child.kind]||transferName(child),transferDetail(child),'$transfer-open',{id:child.id,parent:job.id});
      if (job.status === 'interrupted' && !checking) row('Check result', 'Read the existing result without repeating the operation.', 'job.reconcile', { id: job.id });
      // A timed-out operation is unresolved too, and keeps its resource reserved.
      if (job.status === 'uncertain' && !checking) row('Check result', 'Read the existing result without repeating the operation.', 'job.reconcile', { id: job.id });
      if (['interrupted', 'uncertain'].includes(job.status) && !checking)
        row('Release reservation', 'Give up the unknown result so its resource can be used again.', '$job-release', { id: job.id }, { confirm: true });
      if (job.status === 'held') row('Resume', 'Continue this paused operation.', 'job.resume', { id: job.id }, { confirm: true });
      if (['queued', 'held'].includes(job.status) || job.status === 'running' && handlers.get(job.kind)?.cancellable !== false)
        row('Cancel transfer', 'Keep original files. Completed changes are not undone.', 'job.cancel', { id: job.id }, { confirm: true });
    }
    row(view.parent?'Back to package transfer':'Back to transfers', '', '$transfer-back');return rows;
  }
  const children=new Set(jobs.flatMap(job=>packageSteps(job,jobs).map(child=>child.id)));
  const visible = jobs.filter(j => fileKinds.has(j.kind)&&!children.has(j.id)&&(view.section==='completed'?['completed','cancelled'].includes(j.status):!['completed','cancelled'].includes(j.status)));
  visible.sort((a, b) => Number(live(b.status)) - Number(live(a.status)) || b.updatedAt - a.updatedAt);
  for (const job of visible) row(transferName(job), transferDetail(job, state.saves?.recovering?.includes(job.id)), '$transfer-open', { id: job.id });
  if (!visible.length) row(view.section==='completed'?'No completed transfers':'No active transfers', 'File transfers and installation progress appear here.');
  row('Back to transfers','','$transfer-section',{section:null});
  return rows;
}
