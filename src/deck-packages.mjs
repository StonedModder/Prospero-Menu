// SPDX-License-Identifier: GPL-3.0-or-later
// Snapshot-only controller presentation. No file reads or native probes here.
const terminal = status => ['completed', 'failed', 'cancelled'].includes(status);
const progress = job => {
  const p = job.progress;
  return job.error?.message || job.result?.message || (p?.total > 0 ? `${Math.floor(p.completed * 100 / p.total)}% - ` : '') + (p?.phase || (job.status === 'queued' ? 'Waiting for the current installation to finish.' : job.status));
};
export function packageDeckRows(state, view, jobs) {
  const rows = [], row = (label, detail, command = null, args = {}, extra = {}) =>
    rows.push({ label, detail, command, args, enabled: Boolean(command), ...extra });
  if(view.workflow){
    const job=jobs.find(j=>j.id===view.workflow);
    row(job?.args?.name||'Package installation','Keep USB connected until installation finishes.');
    row(job?.status==='completed'?'Installed':job?.status==='failed'||job?.status==='interrupted'?'Installation needs attention':'Installing package',job?progress(job):'Reading installation progress.');
    const child=jobs.find(j=>j.id===job?.checkpoint?.install||j.id===job?.checkpoint?.stage&& !job?.checkpoint?.install);
    if(child?.status==='held')row('Resume installation','Continue the selected package.','job.resume',{id:child.id});
    if(job?.status==='held')row('Resume installation','Continue the selected package.','job.resume',{id:job.id});
    if(child?.status==='interrupted')row('Check result','Check the existing installation without sending it twice.','job.reconcile',{id:child.id});
    row('Back to packages','Installation continues in the background.','$packages');return rows;
  }
  if (!view.id && !view.downloads && !view.receiver && !view.activity) {
    row('Install from storage', 'Choose a PKG to install. A connected USB drive opens automatically. No extra PKG copy.', '$package-storage');
    row('Downloads','Install or delete PKGs downloaded or received by ProsperoMenu.','$package-downloads');
    row('Receive from phone or PC','Send a PKG or download link using DPI v2.','$package-open',{receiver:true});
    const pending=jobs.filter(j=>['package.install','package.receive'].includes(j.kind)&&!terminal(j.status));
    row('Installation progress',pending.length?`${pending.length} installation operation(s). Check progress or resolve a previous attempt.`:'View recent installations and their results.','$package-open',{activity:true});
    row('Back to home', '', '$package-exit'); return rows;
  }
  if(view.receiver){
    if(state.packageReceiver){
      const receiver=state.packageReceiver;
      row(receiver.enabled?'Stop receiving packages':'Receive packages · DPI v2',receiver.enabled?
        `Send a PKG or download link to ${receiver.address}. Accepted installs continue in the background.`:
        'Turn on the local receiver, then send a PKG from your phone, computer or compatible sender.',
        receiver.closing?null:'package.receiver.set',{enabled:!receiver.enabled},{confirm:!receiver.enabled});
      if(receiver.active)row('Receiving '+receiver.active.name,`${Math.floor(receiver.active.bytes/1048576)} MiB received`);
      if(receiver.last?.error)row('Receiver needs attention',receiver.last.error.message);
    }
    row('Back to packages','','$packages');return rows;
  }
  if(view.activity){
    const children=new Set(jobs.filter(j=>j.kind==='package.receive').map(j=>j.checkpoint?.install));
    const recent=jobs.filter(j=>j.kind==='package.receive'||j.kind==='package.install'&&!children.has(j.id)).slice(-12).reverse();
    for(const job of recent){
      const item=state.packages?.find(p=>p.id===job.args?.id);
      row(item?.name||job.args?.name||'Package',progress(job),job.kind==='package.receive'||item?'$package-open':null,job.kind==='package.receive'?{workflow:job.id}:{id:item?.id});
    }
    if(!recent.length)row('No installations yet','Choose Install from storage or Downloads to begin.');
    row('Back to packages','','$packages');return rows;
  }
  if(view.downloads&&!view.id){
    const downloads=(state.packages||[]).filter(item=>!item.external);
    for(const item of downloads)row(item.name,Math.ceil(item.bytes/1048576)+' MiB · '+item.status,'$package-open',{id:item.id});
    if(!downloads.length)row('No downloads yet','Packages received through DPI v2 appear here. Files selected from USB stay on USB.');
    row('Back to packages','','$packages');return rows;
  }
  const item = state.packages?.find(item => item.id === view.id);
  if (!item) { row('Package no longer available', 'Return to Packages and choose the file again.'); }
  else {
    const related = jobs.filter(job => job.resources?.includes('package-file:' + item.id) || job.kind === 'file.import' && job.result?.id === item.id);
    const active = related.filter(job => !terminal(job.status));
    const verified = jobs.findLast(job => job.status === 'completed' && (job.kind === 'package.verify' && (job.args?.id === item.id || job.resources?.includes('package-file:'+item.id)) ||
      ['package.download','file.import'].includes(job.kind) && job.result?.id === item.id));
    const proof = verified?.result, info = proof?.package;
    const evidenced = /^[a-f0-9]{64}$/.test((item.external && info?.verification==='header' ? info.headerSha256 : proof?.sha256) || '') && typeof info?.contentId === 'string';
    row(item.name, `${Math.ceil(item.bytes / 1048576)} MiB · ${item.external ? item.source + '. Keep the drive connected until installation finishes.' : 'Stored in ProsperoMenu Downloads.'}`);
    if (item.offset < item.bytes) row('Transfer incomplete', 'The transfer is incomplete. Resume it from the device sending this file.');
    else if (item.status === 'uploading' || (item.status === 'verified' && !evidenced))
      row('Verify package', active.length ? 'An operation already holds this file.' : 'Read the complete file and check its checksum in the background.',
        active.length ? null : 'job.enqueue', { kind: 'package.verify', input: { id: item.id } });
    if (item.status === 'verified' && proof) {
      const capability = state.capabilities.find(item => item.id === 'packages');
      const reason = active.length ? 'An operation already holds this file. Review its result below.' : state.recovery?.running
        ? 'Startup is still running. Installation becomes available when it finishes.' : state.safe
        ? 'Startup needs recovery: ' + (state.recovery?.last?.error?.message || 'Review startup to continue.')
        : !capability?.available ? capability?.reason || 'Package installation is unavailable.' : '';
      if (info?.contentId) row(info.titleId || info.contentId, `${info.platform?.toUpperCase() || ''} · ${info.patch ? 'Update' : 'Application'}`);
      if(!active.length)row('Install package', reason || (!evidenced ? 'Run package verification again before installing.' : item.external ? 'Install directly from the selected file. Keep USB connected until finished; no extra PKG copy is made.' : 'Install this package in the background. Progress will appear here.'),
        !reason && evidenced ? 'job.enqueue' : null,
        { kind: 'package.install', input: { id: item.id, sha256: proof.sha256, contentId: info?.contentId, bytes: item.bytes } },
        { confirm: true, packageReview: item.id });
      if(state.safe && state.recovery && !state.recovery.running)
        row('Review startup',state.recovery.last?.error?.message || 'Check services so installation can continue.','$startup-review');
    }
    for (const job of active) {
      row(job.kind === 'package.verify' ? 'Checking package' : 'Installing package', progress(job));
      if (job.status === 'held') row('Resume operation', 'Continue this held job through the shared queue.', 'job.resume', { id: job.id }, { confirm: true });
      if (['queued', 'held'].includes(job.status) || job.kind === 'package.verify' && job.status === 'running')
        row('Cancel operation', job.kind === 'package.verify' ? 'Keep uploaded bytes. Stop checksum verification.' : 'Cancel before the installer starts.', 'job.cancel', { id: job.id });
      if (job.status === 'interrupted') row('Check result', 'Read existing evidence. Never submit another installation.', 'job.reconcile', { id: job.id });
    }
    row(item.external?'Remove from list':'Delete download',active.length?'Finish or cancel this file’s operation first.':item.external?'Remove this entry only. The original PKG stays on its drive.':'Delete this PKG download. The installed application stays installed.',active.length?null:'download.delete',{kind:'package',id:item.id},{confirm:true});
    const latest = related.at(-1);
    if (latest && terminal(latest.status)) row('Last operation: ' + latest.status, latest.error?.message || latest.result?.message || 'See Activity for details.');
  }
  row(item?.external?'Back to packages':'Back to downloads', '', item?.external?'$packages':'$package-downloads'); return rows;
}
