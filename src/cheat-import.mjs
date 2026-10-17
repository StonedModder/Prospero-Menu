// SPDX-License-Identifier: GPL-3.0-or-later
// MC4 envelope interoperability: etaHEN / LightningMods, mc4decrypter.c.
// These published constants describe the file format, not user credentials.
import { invariant, PmError } from './errors.mjs';
import { importCheatSource, importCheatXml } from './cheat-xml.mjs';
const key = '304c6528f659c766110239a51cl5dd9c';
const iv = 'u@}kzW2u[u(8DWar';

export async function importCheatFile(source, identity) {
  invariant(typeof source === 'string' && Buffer.byteLength(source) <= 4 * 1024 * 1024,
    'CHEAT_SIZE', 'Cheat files are limited to 4 MiB.');
  const start = source.replace(/^\ufeff/, '').trimStart();
  if (start.startsWith('{') || start.startsWith('<')) return importCheatSource(source, identity);
  const encoded = start.replace(/[\t\r\n ]/g, '');
  invariant(encoded.length >= 24 && encoded.length % 4 === 0 && /^[A-Za-z0-9+/]*={0,2}$/.test(encoded),
    'CHEAT_MC4', 'Choose a JSON, SHN/XML or base64 MC4 cheat file.');
  const ciphertext = Buffer.from(encoded, 'base64');
  invariant(ciphertext.length % 16 === 0 && ciphertext.toString('base64') === encoded,
    'CHEAT_MC4', 'MC4 data is truncated or has invalid base64 padding.');
  // AES tables are initialized only on the first MC4 import, not at HEN startup.
  const { cbc } = await import('@noble/ciphers/aes.js');
  let plaintext;
  try { plaintext = Buffer.from(cbc(Buffer.from(key), Buffer.from(iv)).decrypt(ciphertext)); }
  catch { throw new PmError('CHEAT_MC4', 'MC4 decryption failed. Check that the file is complete and uses the supported MC4 format.'); }
  let xml;
  if (plaintext[0] === 0xff && plaintext[1] === 0xfe) {
    const content = plaintext.subarray(2);
    invariant(content.length % 2 === 0, 'CHEAT_MC4_TEXT', 'MC4 UTF-16 text is incomplete.');
    xml = content.toString('utf16le');
    invariant(Buffer.from(xml, 'utf16le').equals(content), 'CHEAT_MC4_TEXT', 'MC4 text encoding is invalid.');
  } else {
    xml = plaintext.toString('utf8');
    invariant(Buffer.from(xml, 'utf8').equals(plaintext), 'CHEAT_MC4_TEXT', 'MC4 text must use UTF-8 or BOM-marked UTF-16LE.');
  }
  // Older trainer exports wrap the ENTIRE XML document in escaped markup.
  // Do not rewrite ordinary XML entities inside an already plain document.
  if (xml.replace(/^\ufeff/, '').trimStart().startsWith('&lt;'))
    xml = xml.replace(/&lt;|&gt;|\\&quot;/g, token => token === '&lt;' ? '<' : token === '&gt;' ? '>' : '"');
  // Public MC4 exports can declare utf-16 even though their decrypted bytes
  // are UTF-8. Bytes were decoded explicitly above; normalize that declaration.
  xml = xml.replace(/^(\ufeff?\s*<\?xml\s[^?]*?\bencoding\s*=\s*)(["'])utf-16\2/i, '$1$2utf-8$2');
  return importCheatXml(xml, identity, { legacyTrainer: true });
}
