// SPDX-License-Identifier: GPL-3.0-or-later
import { readFile } from 'node:fs/promises';
import { firmwareProfileFiles } from './firmware-profile-files.mjs';
for (const [file,content] of await firmwareProfileFiles()) {
  if ((await readFile(new URL('../'+file,import.meta.url),'utf8')).replaceAll('\r\n','\n') !== content)
    throw Error('Firmware source drift: regenerate '+file);
}
