// SPDX-License-Identifier: GPL-3.0-or-later
export const dpiPage = `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ProsperoMenu · Install packages</title>
<style>body{max-width:680px;margin:40px auto;padding:0 20px;background:#101113;color:#fff;font:17px system-ui}h1{font-size:28px}form{padding:20px;border:1px solid #666;border-radius:12px;margin:20px 0}input,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin:8px 0}button{background:#fff;color:#111;border:0;border-radius:6px;cursor:pointer}p{line-height:1.5;color:#ddd}progress{width:100%}#status{white-space:pre-wrap}a{color:#fff}</style></head><body>
<h1>Install packages</h1><p>Send a PKG from your device or use a direct download link. ProsperoMenu verifies the file and installs it in the background.</p>
<form id="upload"><label>Package file<input type="file" name="file" accept=".pkg" required></label><button>Upload and install</button></form>
<form id="link"><label>Package download link<input type="url" name="url" placeholder="https://…/game.pkg" required></label><button>Download and install</button></form>
<progress id="progress" max="100" value="0"></progress><p id="status" role="status">Choose a package to begin.</p><p id="result"></p>
<script>const status=document.querySelector('#status'),bar=document.querySelector('#progress');let job=null;
for(const form of document.forms)form.onsubmit=e=>{e.preventDefault();const request=new XMLHttpRequest();request.open('POST','/');
for(const button of document.querySelectorAll('button'))button.disabled=true;status.textContent='Sending package request…';
request.upload.onprogress=e=>{if(e.lengthComputable){bar.value=e.loaded/e.total*100;status.textContent='Uploading '+Math.floor(bar.value)+'%';}};
request.onload=()=>{for(const button of document.querySelectorAll('button'))button.disabled=false;status.textContent=request.responseText;job=request.getResponseHeader('X-ProsperoMenu-Job');};
request.onerror=()=>{for(const button of document.querySelectorAll('button'))button.disabled=false;status.textContent='Connection lost. Check Transfers on the console before sending again; an accepted install may still be running.';};
request.send(new FormData(form));};
setInterval(async()=>{if(!job)return;try{const data=await(await fetch('/status',{cache:'no-store'})).json(),item=data.jobs.find(x=>x.id===job);if(!item)return;status.textContent=item.error?.message||item.result?.message||item.progress?.phase||item.status;if(item.progress?.total)bar.value=item.progress.completed/item.progress.total*100;if(['completed','failed','cancelled'].includes(item.status))job=null;}catch{}},1500);
</script></body></html>`;
