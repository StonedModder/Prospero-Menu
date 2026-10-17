// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoSettings persistence: /data/ProsperoMenu/settings.json.
// Theme colours are RGBA8 hex strings ("00FFDDFF") in the order the deck frame
// carries them (deck-core.h PM_THEME_*).

export const THEME_SLOTS = Object.freeze([
  { key: 'accent', label: 'Highlight', detail: 'Focus outline, toggles that are on and the scrollbar accent.' },
  { key: 'background', label: 'Panel background', detail: 'The docked panel. Lower opacity shows more of the screen behind it.' },
  { key: 'text', label: 'Text', detail: 'Row labels, the title and the button hints.' },
  { key: 'muted', label: 'Secondary text', detail: 'Subtitle, values, category headers and descriptions.' },
  { key: 'separator', label: 'Row separators', detail: 'The thin lines between rows.' },
  { key: 'scrollbar', label: 'Scrollbar', detail: 'The scrollbar handle beside long lists.' },
  { key: 'edge', label: 'Panel edge', detail: 'The 1 px line on the panel\'s right edge.' },
  { key: 'focusFill', label: 'Focused row fill', detail: 'Inside the highlight outline of the focused row.' },
  { key: 'title', label: 'Title text', detail: 'The menu title at the top of the panel.' },
  { key: 'logo', label: 'Logo', detail: 'The mark left of the title. Select a colour for it here.' }
]);
// The built-in palette (deck-layout.c tables): Tesla's highlight on black.
export const DEFAULT_THEME = Object.freeze({
  accent: '00FFDDFF', background: '000000F2', text: 'F7F7F7FF', muted: 'B3B3B3FF',
  separator: 'FFFFFF21', scrollbar: '555555FF', edge: '222222FF', focusFill: '0B0B0BFF',
  title: 'F7F7F7FF', logo: '00FFDDFF'
});
export const PRESETS = Object.freeze([
  { name: 'Tesla teal (default)', theme: DEFAULT_THEME },
  { name: 'PlayStation blue', theme: { ...DEFAULT_THEME, accent: '0070F0FF', focusFill: '061226FF', scrollbar: '2A4D7AFF' } },
  { name: 'Light blue', theme: { ...DEFAULT_THEME, accent: '6EC6FFFF', focusFill: '0A1620FF', scrollbar: '3C6E8FFF' } },
  { name: 'Purple', theme: { ...DEFAULT_THEME, accent: 'B07CFFFF', focusFill: '150B24FF', scrollbar: '5B4380FF' } },
  { name: 'Crimson', theme: { ...DEFAULT_THEME, accent: 'FF3B5CFF', focusFill: '22080DFF', scrollbar: '7A2A38FF' } },
  { name: 'Amber', theme: { ...DEFAULT_THEME, accent: 'FFB020FF', focusFill: '231806FF', scrollbar: '7A5A20FF' } },
  { name: 'Emerald', theme: { ...DEFAULT_THEME, accent: '2EE59DFF', focusFill: '07201AFF', scrollbar: '2A6B52FF' } },
  { name: 'Monochrome', theme: { ...DEFAULT_THEME, accent: 'FFFFFFFF', focusFill: '111111FF', scrollbar: '777777FF' } },
  { name: 'Light panel', theme: { accent: '0070F0FF', background: 'F2F2F2EB', text: '111111FF', muted: '555555FF', separator: '00000026', scrollbar: '999999FF', edge: 'CCCCCCFF', focusFill: 'FFFFFFFF' } }
]);
// Quick picks in the colour picker.
export const SWATCHES = Object.freeze([
  ['Teal', '00FFDD'], ['Blue', '0070F0'], ['Light blue', '6EC6FF'], ['Purple', 'B07CFF'], ['Pink', 'FF5FD2'],
  ['Red', 'FF3B5C'], ['Orange', 'FF7A1A'], ['Amber', 'FFB020'], ['Green', '2EE59D'], ['White', 'FFFFFF'],
  ['Gray', '808080'], ['Black', '000000']
]);
const HEX = /^[0-9A-F]{8}$/;

export function normalizeTheme(raw) {
  const theme = { ...DEFAULT_THEME };
  for (const { key } of THEME_SLOTS) {
    const value = String(raw?.[key] ?? '').replace(/^#/, '').toUpperCase();
    if (HEX.test(value)) theme[key] = value; else if (/^[0-9A-F]{6}$/.test(value)) theme[key] = value + 'FF';
  }
  return theme;
}
export const isDefaultTheme = theme => THEME_SLOTS.every(({ key }) => theme[key] === DEFAULT_THEME[key]);
export const themeColors = theme => THEME_SLOTS.map(({ key }) => parseInt(theme[key], 16) >>> 0);
export const channels = hex => [0, 2, 4, 6].map(i => parseInt(hex.slice(i, i + 2), 16));
export const toHex = values => values.map(v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0')).join('').toUpperCase();

export class ProsperoSettings {
  constructor({ fs, file = '/data/ProsperoMenu/settings.json', log = () => {} }) {
    this.fs = fs; this.file = file; this.log = log;
    this.theme = { ...DEFAULT_THEME }; this.serial = 0; this.writing = Promise.resolve();
  }
  async load() {
    try {
      const raw = JSON.parse(await this.fs.readFile(this.file, 'utf8'));
      this.theme = normalizeTheme(raw.theme);
    } catch (error) { if (error.code !== 'ENOENT') this.log('ProsperoSettings: using defaults, ' + error.message); }
    this.serial = isDefaultTheme(this.theme) ? 0 : 1;
    return this;
  }
  // Theme for the deck frame: serial 0 keeps the native built-in palette.
  frameTheme() { return this.serial ? { serial: this.serial, colors: themeColors(this.theme) } : null; }
  setColor(key, hex) {
    if (!THEME_SLOTS.some(s => s.key === key) || !HEX.test(hex)) return;
    if (this.theme[key] === hex) return;
    this.theme = { ...this.theme, [key]: hex }; this.changed();
  }
  setTheme(theme) { this.theme = normalizeTheme(theme); this.changed(); }
  changed() {
    this.serial = isDefaultTheme(this.theme) ? 0 : ((this.serial + 1) >>> 0 || 1);
    const body = JSON.stringify({ version: 1, theme: this.theme }, null, 1);
    // Writes are serialized; the latest theme always lands last.
    this.writing = this.writing.then(() => this.fs.writeFile(this.file, body)).catch(error => this.log('ProsperoSettings: save failed, ' + error.message));
  }
}
