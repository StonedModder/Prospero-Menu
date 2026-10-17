// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoMenu's root list and its own pages, driven by ConsoleDeck.
//
// The root list is built from /data/ProsperoMenu/overlays. The only entry that
// is not an overlay is ProsperoSettings, which is always last.
import { BUILTIN_PAGES, OVERLAY_API } from './manifest.mjs';
import { parseMenu, MenuOverlay } from './menu-overlay.mjs';
import { THEME_SLOTS, PRESETS, SWATCHES, DEFAULT_THEME, channels, toHex } from './settings.mjs';
import { describeBootAnimation, BOOTANIM_FOLDER } from '../boot-animation.mjs';

export const SETTINGS_LABEL = 'ProsperoSettings';
const VERSION = '1.1.0';
// Without an overlay folder (unit tests, the PC development host) the root
// falls back to ProsperoMenu's own pages so every feature stays reachable.
const FALLBACK = [
  { id: 'packages', name: 'Packages', kind: 'builtin', page: 'packages', description: 'Install PKG files from USB or console storage.' },
  { id: 'games', name: 'Games', kind: 'builtin', page: 'games', description: 'Play installed games and mount game sources.' },
  { id: 'app-dumper', name: 'App Dumper', kind: 'builtin', page: 'dumper', description: 'Copy a mounted title to a USB drive and watch the progress.' },
  { id: 'elf-updater', name: 'Elf-Updater', kind: 'builtin', page: 'elf-updater', description: 'Update payloads from GitHub and arrange the startup order.' }
];

export const shellPath = file => file.startsWith('/data/') ? '/user' + file : file;

export class ProsperoRoot {
  constructor(runtime) {
    this.runtime = runtime; this.location = null; this.suspended = null; this.returnFocusId = null;
    this.rootFocus = null; this.rootFocusAt = 0; this.notice = '';
    runtime.overlays?.host?.onExit((id, message) => {
      if (this.location?.view === 'overlay' && this.location.id === id) this.toRoot(id, message);
    });
  }
  get overlays() { return this.runtime.overlays || null; }
  get settings() { return this.runtime.overlays?.settings || null; }
  list() { return this.overlays ? this.overlays.registry.overlays.filter(o => !o.hidden) : FALLBACK; }
  active() { return Boolean(this.location); }
  theme() { return this.settings?.frameTheme() || null; }
  // The replaceable header logo, discovered by the overlay registry (empty
  // while no logo file exists, which keeps the drawn mark).
  // ShellUI loads the picture itself, and its sandbox has no /data: the same
  // folder is mounted there as /user/data.
  logoPath() {
    // Boot animation: return the current frame's BMP while playing.
    const vid = this.videoSerial();
    if (vid > 0 && this._bootFrames > 0) {
      const idx = vid - 1;
      return shellPath('/data/ProsperoMenu/autoloadAnims/frame_' + String(idx).padStart(3, '0') + '.bmp');
    }
    return shellPath(this.overlays?.registry.logo || '');
  }
  // Boot animation: frame counter drives both the image path and the reload
  // trigger. The deck reloads whenever videoSerial changes (deck-view.c), so
  // this is frame-accurate rather than a timing guess.
  videoSerial() {
    if (this._bootFrames > 0 && this._bootStart) {
      const elapsed = Date.now() - this._bootStart;
      const idx = Math.floor(elapsed / 33); // ~30fps
      if (idx < this._bootFrames) return idx + 1;
      this._bootFrames = 0; // animation done
    }
    return 0;
  }
  // Called once at startup after the C code writes frame BMPs to disk.
  startBootAnimation(frameCount) {
    this._bootFrames = frameCount;
    this._bootStart = Date.now();
    console.log('ProsperoMenu: boot animation started, ' + frameCount + ' frames');
  }
  refresh() { this.overlays?.rescan().catch(() => {}); }
  toRoot(fromId, notice = '') {
    this.location = null;
    const index = fromId === SETTINGS_LABEL ? this.list().length + (this.overlays?.registry.broken.length || 0) : this.list().findIndex(o => o.id === fromId);
    this.rootFocus = index >= 0 ? index : null; this.rootFocusAt = Date.now(); this.notice = notice || '';
  }

  // ---- root list ---------------------------------------------------------
  rootRows() {
    const rows = [];
    for (const overlay of this.list()) rows.push({ label: overlay.name, value: overlay.kind === 'builtin' ? '' : overlay.version ? 'v' + overlay.version : '',
      detail: overlay.description || (overlay.kind === 'builtin' ? 'Built-in page' : ''), enabled: true, command: '$pm-open', args: { id: overlay.id } });
    for (const broken of this.overlays?.registry.broken || [])
      rows.push({ label: broken.name || broken.id, value: 'Error', detail: broken.error, enabled: false });
    if (!rows.length) rows.push({ label: 'No overlays installed', detail: 'Copy overlay folders into /data/ProsperoMenu/overlays/ (FTP or the file browser), then open the menu again.', enabled: false });
    rows.push({ label: SETTINGS_LABEL, detail: 'Theme colours, overlays and console menu settings.', enabled: true, command: '$pm-settings', args: { page: 'main' } });
    return rows;
  }

  // ---- views -------------------------------------------------------------
  view(state) {
    const loc = this.location;
    if (loc.view === 'overlay') {
      const overlay = this.overlays.registry.get(loc.id);
      if (!overlay) return { title: 'Overlay removed', rows: [{ label: 'This overlay is no longer installed', detail: 'Press Circle to return to the list.', enabled: false }] };
      return this.overlays.host.view(overlay);
    }
    if (loc.view === 'menu') return loc.menu.view();
    if (loc.view === 'error') return { title: loc.title, rows: [{ label: 'Overlay could not open', detail: loc.message, enabled: false }] };
    return this.settingsView(state, loc);
  }
  settingsView(state, loc) {
    const rows = [], row = (label, detail, command = null, args = {}, extra = {}) => rows.push({ label, detail, command, args, enabled: Boolean(command), ...extra });
    // Plain information: drawn like any other row (not in the warning colour
    // that explains why a row is unavailable); X does nothing.
    const info = (label, detail, extra = {}) => row(label, detail, '$pm-none', {}, extra);
    const header = label => rows.push({ label, kind: 'header', enabled: false });
    const s = this.settings;
    if (loc.page === 'main') {
      header('About');
      info('ProsperoMenu', 'Tesla-style overlay menu for PlayStation 5.', { value: 'v' + VERSION });
      row('Created by StonedModder', '', null);
      info('Overlay API', 'Overlays speak JSON lines over 127.0.0.1. See docs/OVERLAYS.md.', { value: String(OVERLAY_API) });
      header('Appearance');
      row('Theme colours', 'Pick each colour of the menu with a colour picker.', '$pm-settings', { page: 'colors' }, { value: s ? '#' + s.theme.accent.slice(0, 6) : '' });
      row('Theme presets', 'Start from a ready-made palette, then fine-tune it in Theme colours.', '$pm-settings', { page: 'presets' });
      row('Reset colours', 'Return to the built-in palette.', s ? '$pm-theme-reset' : null);
      header('Overlays');
      const registry = this.overlays?.registry;
      if (registry) info('Overlay folder', registry.dir + '/', { value: registry.overlays.length + ' installed' });
      else row('Overlay folder', 'Not available on this host');
      row('Reload overlays', 'Read the overlay folder again after copying or editing overlays.', this.overlays ? '$pm-rescan' : null);
      row('Running overlays', 'See and stop overlay processes.', this.overlays ? '$pm-settings' : null, { page: 'running' });
      row('Restore example overlays', 'Write the bundled examples into the overlay folder again, replacing their files.', this.overlays ? '$pm-restore' : null, {}, { confirm: true });
      header('Boot animation');
      info('MP4 folder', BOOTANIM_FOLDER + ' · drop one or more MP4 (H.264/AAC) videos here; more than one is chosen at random');
      row('ELF loader boot animation', describeBootAnimation(!!state.settings?.bootAnimation?.enabled, state.bootAnimation?.count ?? 0), 'settings.update',
        { revision: state.revision, patch: { bootAnimation: { enabled: !state.settings?.bootAnimation?.enabled } } }, { on: !!state.settings?.bootAnimation?.enabled });
      row('Preview animation', state.bootAnimation?.playing ? 'Playing now — pressing it again is refused until it finishes' : 'Play one now, chosen at random from the folder.',
        state.bootAnimation ? '$pm-preview-anim' : null);
      header('Console');
      row('Console menu settings', 'FPS and resource readouts, IP display, reduced motion and services.', '$page', { page: 4 });
      row('File browser', 'Copy files and folders between console storage and USB.', '$file-manager');
      return { title: SETTINGS_LABEL, subtitle: 'ProsperoMenu v' + VERSION, rows };
    }
    if (loc.page === 'colors') {
      for (const slot of THEME_SLOTS) row(slot.label, slot.detail, s ? '$pm-settings' : null, { page: 'picker', slot: slot.key },
        { value: s ? '#' + s.theme[slot.key].slice(0, 6) + (s.theme[slot.key].endsWith('FF') ? '' : ' ' + Math.round(parseInt(s.theme[slot.key].slice(6), 16) / 2.55) + '%') : '' });
      return { title: 'Theme colours', subtitle: 'Changes apply immediately', rows };
    }
    if (loc.page === 'presets') {
      for (const preset of PRESETS) row(preset.name, 'Highlight #' + preset.theme.accent.slice(0, 6) + '. Select to apply.', s ? '$pm-preset' : null, { name: preset.name },
        { on: s && THEME_SLOTS.every(({ key }) => s.theme[key] === (preset.theme[key] ?? DEFAULT_THEME[key])) });
      return { title: 'Theme presets', subtitle: 'Then fine-tune in Theme colours', rows };
    }
    if (loc.page === 'picker') {
      const slot = THEME_SLOTS.find(x => x.key === loc.slot), hex = s.theme[slot.key], [r, g, b, a] = channels(hex);
      header('Colour  #' + hex.slice(0, 6) + '  opacity ' + Math.round(a / 2.55) + '%');
      for (const [name, value, channel] of [['Red', r, 0], ['Green', g, 1], ['Blue', b, 2]])
        row(name, 'Left/Right changes by 5, hold to repeat. X jumps by 51.', '$pm-channel', { slot: slot.key, channel, step: 51 }, { value: `< ${value} >`, adjust: true });
      row('Opacity', 'Left/Right changes by 5%. 100% is fully opaque.', '$pm-channel', { slot: slot.key, channel: 3, step: 13 }, { value: `< ${Math.round(a / 2.55)}% >`, adjust: true });
      header('Swatches');
      for (const [name, rgb] of SWATCHES) row(name, '#' + rgb, '$pm-swatch', { slot: slot.key, rgb }, { on: hex.slice(0, 6) === rgb, value: '#' + rgb });
      row('Default', 'The built-in colour for this slot: #' + DEFAULT_THEME[slot.key].slice(0, 6), '$pm-swatch', { slot: slot.key, rgba: DEFAULT_THEME[slot.key] });
      return { title: slot.label, subtitle: slot.detail.slice(0, 80), footer: 'X Select   Circle Back   Left/Right Adjust', rows };
    }
    if (loc.page === 'running') {
      const sessions = this.overlays?.host.snapshot() || [];
      for (const session of sessions) row(this.overlays.registry.get(session.id)?.name || session.id, session.message || (session.unresponsive ? 'Not responding' : 'Running'),
        session.state === 'connected' || session.state === 'starting' ? '$pm-stop' : null, { id: session.id }, { value: session.state === 'connected' ? (session.pid > 0 ? 'pid ' + session.pid : 'running') : session.state });
      if (!sessions.length) row('No overlay processes', 'Overlays start when you open them, or at startup when they ask for it.');
      return { title: 'Running overlays', subtitle: 'X stops an overlay', rows };
    }
    return { title: SETTINGS_LABEL, rows };
  }

  // ---- actions -------------------------------------------------------------
  // Returns a ProsperoMenu {command,args} to run instead (builtin pages), or null.
  async select(selected) {
    const { command, args } = selected, s = this.settings;
    if (command === '$pm-none') return null;
    if (command === '$pm-open') return this.open(args.id);
    if (command === '$pm-settings') {
      const parent = this.location?.view === 'settings' ? this.location : null;
      this.location = { view: 'settings', page: args.page, slot: args.slot, parent }; return null;
    }
    if (command === '$pm-overlay') { this.overlays.host.event(args.id, 'select'); return null; }
    if (command === '$pm-menu') {
      const action = this.location.menu.select(args.id);
      if (!action) return null;
      if (action.type === 'overlay') return this.open(action.id);
      if (action.type === 'notify') { this.overlays?.notify(action.text || 'Done'); return null; }
      if (action.type === 'payload') { await this.overlays.runPayload(this.location.overlay, action.file); return null; }
      return null;
    }
    if (command === '$pm-theme-reset') { s.setTheme(DEFAULT_THEME); return null; }
    if (command === '$pm-preset') { s.setTheme(PRESETS.find(p => p.name === args.name).theme); return null; }
    if (command === '$pm-swatch') { s.setColor(args.slot, args.rgba || (args.rgb + s.theme[args.slot].slice(6))); return null; }
    if (command === '$pm-channel') { this.adjustChannel(args, args.step); return null; }
    if (command === '$pm-preview-anim') return { command: 'bootanim.preview', args: {} };
    if (command === '$pm-rescan') { await this.overlays.rescan(); this.notice = this.overlays.registry.overlays.length + ' overlays found'; return null; }
    if (command === '$pm-restore') { await this.overlays.restoreExamples(); this.notice = 'Example overlays restored'; return null; }
    if (command === '$pm-stop') { this.overlays.host.stop(args.id); return null; }
    return null;
  }
  adjust(selected, direction) {
    if (selected.command === '$pm-overlay') return this.overlays.host.event(selected.args.id, direction);
    if (selected.command === '$pm-channel') { this.adjustChannel(selected.args, direction === 'next' ? 5 : -5); return true; }
    return false;
  }
  adjustChannel({ slot, channel }, delta) {
    const s = this.settings, values = channels(s.theme[slot]);
    // X wraps the coarse step; Left/Right clamp at 0 and 255.
    values[channel] = Math.abs(delta) > 5 ? (values[channel] + delta > 255 ? 0 : values[channel] + delta) : Math.max(0, Math.min(255, values[channel] + delta));
    s.setColor(slot, toHex(values));
  }
  async open(id) {
    const overlay = this.list().find(o => o.id === id);
    if (!overlay) return null;
    if (overlay.kind === 'builtin') { this.rootFocus = null; return { ...BUILTIN_PAGES[overlay.page], fromOverlay: id }; }
    if (overlay.kind === 'menu') {
      try {
        const text = await this.overlays.fs.readFile(overlay.folder + '/' + overlay.menu, 'utf8');
        this.location = { view: 'menu', id, overlay, menu: new MenuOverlay(overlay, parseMenu(text)) };
      } catch (error) { this.location = { view: 'error', id, title: overlay.name, message: error.message }; }
      return null;
    }
    this.location = { view: 'overlay', id };
    await this.overlays.host.open(overlay);
    return null;
  }
  async back() {
    const loc = this.location;
    if (loc.view === 'settings') { if (loc.parent) this.location = loc.parent; else this.toRoot(SETTINGS_LABEL); return; }
    if (loc.view === 'menu') { if (!loc.menu.back()) this.toRoot(loc.id); return; }
    if (loc.view === 'overlay') {
      if (this.overlays.host.back() === 'exit') { this.overlays.host.leave(); this.toRoot(loc.id); }
      return;
    }
    this.toRoot(loc.id);
  }
  setVisible(visible) {
    this.overlays?.host.setVisible(visible && this.location?.view === 'overlay');
    if (visible) this.refresh();
  }
}
