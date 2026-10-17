// SPDX-License-Identifier: GPL-3.0-or-later
// File-manager presentation is separate from menu rows. Both panes remain visible.
export const BROWSER_ROWS=12;
const PAGE=9;
export const fileSize=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KiB':n<1073741824?(n/1048576).toFixed(1)+' MiB':(n/1073741824).toFixed(2)+' GiB';
const action=(label,detail,command,args={},extra={})=>({label,detail,command,args,enabled:!!command,...extra});
export async function openFileManager(runtime){
 const roots=await runtime.execute('files.list',{}),left=roots.entries.find(x=>x.source==='/data'),usb=roots.entries.find(x=>x.source.startsWith('/mnt/usb'));
 return {active:0,selected:null,panes:await Promise.all([left,usb].map(async entry=>({listing:entry?await runtime.execute('files.list',{source:entry.source}):roots,offset:0})))};
}
export async function refreshFileManager(runtime,view){
 for(const pane of view.panes){try{pane.listing=await runtime.execute('files.list',{source:pane.listing.source});}catch(error){pane.listing=await runtime.execute('files.list',{});pane.offset=0;view.message=error.message;}}
 if(view.selected){const list=view.panes[view.selected.pane].listing.entries;const current=list.find(x=>x.source===view.selected.source);if(!current||current.ino!==view.selected.ino||current.modified!==view.selected.modified)view.selected=null;}
}
export function fileManagerFrame(view,jobs=[]){
 const panes=view.panes.map((pane,side)=>{
  const {listing}=pane;pane.offset=Math.min(pane.offset,Math.max(0,Math.floor((listing.entries.length-1)/PAGE)*PAGE));
  const entries=[action(listing.source?'..  Parent folder':'Storage devices',listing.source?'Up one folder':listing.usbMissing?'USB not connected':'Console and USB',listing.source?'$manager-folder':null,{side,source:listing.parent})];
  for(const item of listing.entries.slice(pane.offset,pane.offset+PAGE))entries.push(action(item.name,item.directory?'Folder':fileSize(item.bytes),'$manager-entry',{side,item},{directory:item.directory,on:view.selected?.source===item.source}));
  while(entries.length<10)entries.push(action('','',null));
  entries.push(action('Previous page','',pane.offset?'$manager-page':null,{side,offset:Math.max(0,pane.offset-PAGE)}));
  entries.push(action('Next page',`${Math.min(pane.offset+PAGE,listing.entries.length)} / ${listing.entries.length}`,pane.offset+PAGE<listing.entries.length?'$manager-page':null,{side,offset:pane.offset+PAGE}));
  return {path:listing.source||'Storage devices',entries};
 });
 const selected=view.selected,destination=selected?view.panes[1-selected.pane].listing.source:null;
 const canTransfer=selected&&destination;
 const source=selected?.source||'Select a file or folder with X';
 const target=view.panes[view.active].listing.source;
 const rows=[
  action('Copy',canTransfer?`${source} → ${destination}${selected.directory?' · Includes all files and subfolders.':''}`:'Select a file or folder and open a destination in the other pane.',canTransfer?'job.enqueue':null,{kind:'file.transfer',input:{...selected,destination,move:false}},{confirm:true}),
  action('Move',selected?.directory?'Use Copy for folders.':canTransfer?`${source} → ${destination}. Remove original after verification.`:'Select a file and open a destination in the other pane.',canTransfer&&!selected.directory?'job.enqueue':null,{kind:'file.transfer',input:{...selected,destination,move:true}},{confirm:true}),
  action('Rename',source,selected&&!selected.directory?'$manager-name':null,{operation:'rename'}),
  action('Delete',selected?`Permanently delete ${source}. Directories must be empty.`:'Select a file or folder first.',selected?'job.enqueue':null,{kind:'file.delete',input:selected||{}},{confirm:true}),
  action('New folder',target||'Open a folder first.',target?'$manager-name':null,{operation:'mkdir'}),
  action('Refresh','Read both folders and detect connected USB drives.','$manager-refresh'),
  action('Transfers','View progress or cancel a transfer.','$manager-transfers'),
  action('Close','Return to Transfers.','$manager-exit')
 ];
 const job=jobs.find(x=>x.id===view.job);
 let status=view.message||'';
 if(job){const p=job.progress;status=job.error?.message||job.result?.message||`${job.status}: ${p?.phase||job.args.name||'File operation'}${p?.total?' · '+Math.floor(p.completed*100/p.total)+'%':''}`;}
 return {panes,rows,active:view.active,status,selection:selected?source+(selected.directory?' · X again opens this folder.':` · ${fileSize(selected.bytes)}`):'X selects a file or folder. X again opens a selected folder. Copy sends it to the opposite pane.'};
}
