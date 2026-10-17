// SPDX-License-Identifier: GPL-3.0-or-later
import { writeFile } from 'node:fs/promises';
import { firmwareProfileFiles } from './firmware-profile-files.mjs';
for (const [file,content] of await firmwareProfileFiles()) await writeFile(new URL('../'+file,import.meta.url),content);
console.log('Generated native/JavaScript profiles and append-only update firmware mapping.');
