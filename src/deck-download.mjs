// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
import { downloadUrl } from './download-url.mjs';
import { payloadRunAction } from './deck-payload-action.mjs';
export const KEYBOARDS = ['abcdefghijklmnopqrstuvwxyz0123456789', 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', ':?&=_-%+#@!~;,()[]{}$*0123456789abcd'];
export function editDownload(input, kind, key) {
  if (kind === 'back') return 'cancel';
  if (kind === 'next' || kind === 'previous') { input.page = (input.page + (kind === 'next' ? 1 : 2)) % 3; return; }
  invariant(kind === 'select' && Number.isInteger(key) && key >= 0 && key < 40, 'DECK_ACTION', 'Choose a keyboard key.');
  if (key === 39) {
    if(input.pluginField==='titleId'){
      input.value=input.value.trim().toUpperCase();
      invariant(/^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/.test(input.value),'PLUGIN_TARGET','Enter a title ID such as PPSA00001 or CUSA00001.');
    }else if(input.pluginField==='version')invariant(/^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/.test(input.value),'PLUGIN_TARGET','Use the installed version: 01.010.000 for PS5 or 01.00 for PS4.');
    else if(input.operation)invariant(/^[^/\\\x00-\x1f\x7f]{1,180}$/.test(input.value)&&!['.','..'].includes(input.value)&&input.value.trim()===input.value,'FILE_NAME','Enter a name without slashes.');
    else if (input.field === 'url') downloadUrl(input.value);
    else invariant(/^[^/\\\x00-\x1f]{1,176}\.elf$/i.test(input.value), 'DOWNLOAD_NAME', 'Use a filename ending in .elf.');
    return 'done';
  }
  if (key === 38) { input.value = input.value.slice(0, -1); return; }
  const char = key === 36 ? '.' : key === 37 ? '/' : KEYBOARDS[input.page][key];
  invariant(input.value.length < (input.field === 'url' ? 2048 : 180), 'DOWNLOAD_INPUT_LIMIT', 'This text is too long. Use a shorter direct link or filename.');
  input.value += char;
}
export function suggestedName(url) {
  const name = url.split('?')[0].split('/').pop();
  return /^[a-z0-9._-]{1,176}\.elf$/i.test(name) ? name : 'payload.elf';
}
export function downloadRows(state, view, componentsByHash) {
  const rows=[], row=(label,detail,command=null,args={},extra={})=>rows.push({label,detail,command,args,enabled:!!command,...extra});
  if (view.job) {
    const job=state.jobs.find(j=>j.id===view.job);
    row(job?.status==='completed'?'Payload ready':['failed','interrupted'].includes(job?.status)?'Download needs attention':job?.status==='cancelled'?'Download cancelled':'Downloading payload',
      job?.error?.message || job?.result?.message || job?.progress?.phase || 'Connecting to the download server.');
    if(job?.progress?.total && !job.progress.indeterminate) row('Progress',Math.floor(job.progress.completed*100/job.progress.total)+'%');
    if(job?.status==='completed'){
      const item=state.inbox.find(x=>x.id===job.result.id);
      if(item){const action=payloadRunAction(state,{...item,sha256:job.result.sha256},componentsByHash);
        row('Load payload',action.detail,action.command,action.args,{...action,confirm:false,payloadResult:true,payloadFile:item.id});}
      row('Payload options','Set startup or delete the stored copy.','$payload-file',{id:job.result.id});
    }
    if(['queued','running','held'].includes(job?.status))row('Cancel download','Stop downloading this file.','job.cancel',{id:job.id});
  } else {
    row('Download link',view.url || 'Enter an HTTP or HTTPS direct-download link. A short link is easier to type.','$download-edit',{field:'url'});
    row('Filename',view.name || 'payload.elf','$download-edit',{field:'name'});
    row('Download payload','Save and verify the file in Downloads. Choose Load payload when ready.',view.url?'job.enqueue':null,
      {kind:'payload.download',input:{url:view.url,name:view.name||'payload.elf'}});
  }
  row('Back to payloads','','$download-exit');return rows;
}
