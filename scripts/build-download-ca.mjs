// SPDX-License-Identifier: GPL-3.0-or-later
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
const source = readFileSync(new URL('../vendor/ca/cacert.pem', import.meta.url));
const provenance = JSON.parse(readFileSync(new URL('../vendor/ca/provenance.json', import.meta.url)));
if (createHash('sha256').update(source).digest('hex') !== provenance.sha256) throw Error('Download certificate bundle changed; review its provenance.');
const certs = [...source.toString('ascii').matchAll(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g)].map(m => m[0] + '\n');
if (certs.length !== provenance.certificates) throw Error('Certificate bundle count changed.');
writeFileSync(new URL('../build/download-ca.h', import.meta.url),
  '// Generated from the pinned Mozilla trust bundle. See vendor/ca/provenance.json (MPL-2.0).\n' +
  'static const char *const pm_ca_pem[] = {\n' + certs.map(s => JSON.stringify(s)).join(',\n') + '\n};\n');
