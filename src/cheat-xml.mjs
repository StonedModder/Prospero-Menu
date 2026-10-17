// SPDX-License-Identifier: GPL-3.0-or-later
// Trainer/Cheat/Cheatline fields follow etaHEN's public ParseXMLCheat and
// GoldHEN's SHN repository. This is a bounded data reader, never an XML executor.
import { invariant, PmError } from './errors.mjs';
import { importCheatJson } from './cheats.mjs';
const bad = message => { throw new PmError('CHEAT_XML', message); };
const validChar = n => n === 9 || n === 10 || n === 13 || n >= 32 && n <= 0xd7ff || n >= 0xe000 && n <= 0xfffd || n >= 0x10000 && n <= 0x10ffff;
function entities(text) {
  return text.replace(/&([^;\s<&]{1,32});|&/g, (whole, name) => {
    const builtins = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };
    if (Object.hasOwn(builtins, name)) return builtins[name];
    const n = /^#x[0-9a-fA-F]+$/.test(name) ? parseInt(name.slice(2), 16) : /^#[0-9]+$/.test(name) ? Number(name.slice(1)) : NaN;
    if (!Number.isSafeInteger(n) || !validChar(n)) bad('Unsupported or invalid XML entity in cheat file.');
    return String.fromCodePoint(n);
  });
}
function xml(source) {
  // Some public SHN files retain an AES-block padding suffix. Accept only a
  // complete repeated padding suffix after the closing root. Repository line-ending
  // conversion can change byte alignment after decryption; interior controls and
  // malformed suffixes remain invalid XML.
  const padding = source.charCodeAt(source.length - 1);
  if (padding >= 1 && padding <= 16 && source.endsWith(String.fromCharCode(padding).repeat(padding))) {
    const unpadded = source.slice(0, -padding);
    if (unpadded.trimEnd().endsWith('</Trainer>')) source = unpadded;
  }
  source = source.replace(/^\ufeff/, '');
  for (const char of source) if (!validChar(char.codePointAt(0))) bad('Invalid character in XML cheat file.');
  let at = 0, root = null, count = 0, declaration = false;const stack = [];
  function space() { const start = at;while (/[\t\n\r ]/.test(source[at] || '') && at < source.length) at++;return at > start; }
  function name() { const match = /^[A-Za-z_][A-Za-z0-9_.:-]*/.exec(source.slice(at));if (!match) bad('Invalid XML name.');at += match[0].length;return match[0]; }
  function body(text, literal = false) {
    const value = literal ? text : entities(text), node = stack.at(-1);
    if (!node) { if (value.trim()) bad('Unexpected content outside the Trainer element.');return; }
    if (node.text.length + value.length > 16384) bad('XML cheat field is too long.');node.text += value;
  }
  while (at < source.length) {
    if (source[at] !== '<') {
      const end = source.indexOf('<', at), stop = end < 0 ? source.length : end;
      const value = source.slice(at, stop);if (value.includes(']]>')) bad('Unexpected XML CDATA terminator.');body(value);at = stop;continue;
    }
    if (source.startsWith('<!--', at)) {
      const end = source.indexOf('-->', at + 4);if (end < 0 || source.slice(at + 4, end).includes('--')) bad('Invalid XML comment.');at = end + 3;continue;
    }
    if (source.startsWith('<![CDATA[', at)) {
      const end = source.indexOf(']]>', at + 9);if (end < 0 || !stack.length) bad('Invalid XML CDATA field.');body(source.slice(at + 9, end), true);at = end + 3;continue;
    }
    if (source.startsWith('<?', at)) {
      const end = source.indexOf('?>', at + 2), value = source.slice(at, end + 2);
      if (end < 0 || at !== 0 || declaration || !/^<\?xml\s+version=(?:"1\.0"|'1\.0')(?:\s+encoding=(?:"[Uu][Tt][Ff]-8"|'[Uu][Tt][Ff]-8'))?(?:\s+standalone=(?:"(?:yes|no)"|'(?:yes|no)'))?\s*\?>$/.test(value)) bad('Unsupported XML declaration or processing instruction.');
      declaration = true;at = end + 2;continue;
    }
    if (source.startsWith('<!', at)) bad('DTD and custom entities are not allowed in cheat files.');
    if (source.startsWith('</', at)) {
      at += 2;const closing = name();space();if (source[at++] !== '>' || stack.pop()?.name !== closing) bad('Mismatched XML closing element.');continue;
    }
    at++;const node = { name: name(), attrs: Object.create(null), children: [], text: '' };
    if (++count > 32768 || stack.length >= 8) bad('XML cheat structure exceeds its limits.');
    let attributes = 0;
    for (;;) {
      const separated = space();if (source[at] === '/' || source[at] === '>') break;
      if (!separated || ++attributes > 32) bad('Invalid XML attributes.');
      const key = name();if (Object.hasOwn(node.attrs, key)) bad('Duplicate XML attribute.');space();if (source[at++] !== '=') bad('XML attribute requires a value.');space();
      const quote = source[at++];if (quote !== '"' && quote !== "'") bad('XML attributes must be quoted.');
      const end = source.indexOf(quote, at);if (end < 0 || end - at > 16384 || source.slice(at, end).includes('<')) bad('Invalid XML attribute value.');
      node.attrs[key] = entities(source.slice(at, end));at = end + 1;
    }
    if (stack.length) stack.at(-1).children.push(node);else { if (root) bad('Multiple XML root elements.');root = node; }
    if (source[at] === '/') { at++;if (source[at++] !== '>') bad('Invalid empty XML element.'); }
    else { at++;stack.push(node); }
  }
  if (stack.length || !root) bad('Incomplete XML cheat file.');return root;
}
function attributes(node, allowed) {
  if (Object.keys(node.attrs).some(key => !allowed.includes(key))) bad(`Unsupported ${node.name} attribute.`);
}
function children(node, allowed) {
  if (node.text.trim() || node.children.some(child => !allowed.includes(child.name))) bad(`Unsupported content in ${node.name}.`);
}
function field(node, name, required = false) {
  const values = node.children.filter(child => child.name === name);
  if (values.length > 1 || required && !values.length) bad(`Invalid or missing ${name} field.`);
  if (!values.length) return '';
  const item = values[0];attributes(item, []);if (item.children.length) bad(`Nested content in ${name}.`);return item.text.trim();
}
export function importCheatXml(source, identity, { legacyTrainer = false } = {}) {
  invariant(typeof source === 'string' && Buffer.byteLength(source) <= 4 * 1024 * 1024, 'CHEAT_SIZE', 'Cheat document is too large.');
  // The API receives decoded Unicode text, not encoded XML bytes. Public SHN
  // exports sometimes retain a utf-16 declaration after conversion to UTF-8.
  // Normalize only that declaration; no entities or patch content are changed.
  source = source.replace(/^(\ufeff?<\?xml\s[^?]*?\bencoding\s*=\s*)(["'])utf-16\2/i, '$1$2utf-8$2');
  const root = xml(source);if (root.name !== 'Trainer') bad('Expected an SHN Trainer element.');
  attributes(root, ['Game', 'Process', 'Moder', 'Cusa', 'Version', 'xmlns:xsd', 'xmlns:xsi']);children(root, ['Cheat', 'Genres', 'Items']);
  for (const metadata of root.children.filter(child => child.name !== 'Cheat')) {
    attributes(metadata, metadata.name === 'Genres' ? ['Name'] : []);children(metadata, []);
  }
  const mods = root.children.filter(child => child.name === 'Cheat').map(node => {
    attributes(node, legacyTrainer ? ['Text', 'Description', 'Control'] : ['Text', 'Description']);children(node, ['Cheatline']);
    if (legacyTrainer && node.attrs.Control != null && !['Toggle', 'Toggel'].includes(node.attrs.Control)) bad('This MC4 control is not a toggle cheat.');
    const memory = node.children.map(line => {
      attributes(line, []);children(line, ['Offset', 'Section', 'ValueOn', 'ValueOff', 'Absolute']);
      const on = field(line, 'ValueOn', true), off = field(line, 'ValueOff', true);
      // Do not remove arbitrary punctuation from patch bytes.
      if (![on, off].every(value => /^(?:[a-fA-F0-9]{2})+$/.test(value) || /^[a-fA-F0-9]{2}(?:-[a-fA-F0-9]{2})*$/.test(value))) bad('Invalid SHN patch bytes.');
      const absolute = field(line, 'Absolute').toLowerCase();
      invariant(['', 'true', 'false', '1', '0'].includes(absolute), 'CHEAT_ADDRESS_MODE', 'Absolute must be True or False.');
      return { offset: field(line, 'Offset', true), section: field(line, 'Section'), on: on.replaceAll('-', ''), off: off.replaceAll('-', ''), absolute: absolute === 'true' || absolute === '1' };
    });
    return { name: node.attrs.Text, description: node.attrs.Description, memory };
  });
  const version = legacyTrainer && /^\d\.\d{2}$/.test(root.attrs.Version || '') ? '0' + root.attrs.Version : root.attrs.Version;
  return importCheatJson(JSON.stringify({ name: root.attrs.Game, process: root.attrs.Process, id: root.attrs.Cusa, version,
    credits: root.attrs.Moder ? [root.attrs.Moder] : [], mods }), identity);
}
export function importCheatSource(source, identity) {
  invariant(typeof source === 'string' && Buffer.byteLength(source) <= 4 * 1024 * 1024, 'CHEAT_SIZE', 'Cheat document is too large.');
  const start = source.replace(/^\ufeff/, '').trimStart();
  if (start.startsWith('{')) return importCheatJson(start, identity);
  if (start.startsWith('<')) return importCheatXml(source, identity);
  throw new PmError('CHEAT_FORMAT', 'Expected JSON or SHN/XML cheat text.');
}
