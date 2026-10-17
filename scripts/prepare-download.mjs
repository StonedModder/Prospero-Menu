// SPDX-License-Identifier: GPL-3.0-or-later
import {readFile, mkdir, writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'..');
const source=process.env.PROSPEROMENU_DOWNLOAD_LIBS || path.join(root,'../etahen-13.60/Source Code/lib');
const manifest=JSON.parse(await readFile(path.join(root,'vendor/download/inputs.json')));
await mkdir(path.join(root,'build/download'),{recursive:true});
for(const item of manifest.inputs){
  const target=path.join(root,'build/download',item.file);
  let data;try{data=await readFile(target);}catch(error){if(error.code!=='ENOENT')throw error;data=await readFile(path.join(source,item.file));}
  if(data.length!==item.bytes||createHash('sha256').update(data).digest('hex')!==item.sha256)
    throw Error('Download library does not match reviewed PS5 input: '+item.file);
  await writeFile(target,data);
}
