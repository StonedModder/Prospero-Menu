// SPDX-License-Identifier: GPL-3.0-or-later
import { clone, invariant } from './errors.mjs';
import { requireGame } from './firmware.mjs';
const title = /^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/;
const version = /^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/;
export function validateProfile(input) {
  invariant(input && title.test(input.titleId) && version.test(input.version), 'PROFILE_IDENTITY', 'A loadout must target an exact title and version.');
  invariant(typeof input.name === 'string' && input.name.trim().length > 0 && input.name.length <= 100 && !/[\x00-\x1f\x7f]/.test(input.name),
    'PROFILE_NAME', 'Give the loadout a name of at most 100 characters.');
  invariant(Array.isArray(input.plugins) && input.plugins.length <= 32 && Array.isArray(input.cheats) && input.cheats.length <= 64,
    'PROFILE_SIZE', 'The loadout contains too many plugins or cheats.');
  invariant(input.plugins.every(id => /^[a-f0-9]{64}$/.test(id)) && new Set(input.plugins).size === input.plugins.length,
    'PROFILE_PLUGIN', 'Plugins must be identified by unique SHA-256 content hashes.');
  invariant(input.cheats.every(cheat => cheat && typeof cheat.pack === 'string' && /^[a-f0-9]{64}$/.test(cheat.pack) && typeof cheat.id === 'string' && /^\d{1,4}$/.test(cheat.id)), 'PROFILE_CHEAT', 'Invalid cheat reference.');
  invariant(new Set(input.cheats.map(cheat => cheat.pack + ':' + cheat.id)).size === input.cheats.length, 'PROFILE_CHEAT', 'Duplicate cheat reference.');
  const overlay = {};
  invariant(input.overlay === undefined || input.overlay && typeof input.overlay === 'object' && !Array.isArray(input.overlay), 'PROFILE_OVERLAY', 'Invalid overlay choices.');
  for (const [key, value] of Object.entries(input.overlay || {})) {
    invariant(['fps', 'ram', 'cpu', 'ip'].includes(key) && typeof value === 'boolean', 'PROFILE_OVERLAY', 'Invalid loadout overlay option.'); overlay[key] = value;
  }
  invariant(typeof input.autostart === 'boolean', 'PROFILE_START', 'Choose whether the loadout starts automatically.');
  return { schema: 1, titleId: input.titleId, version: input.version, name: input.name,
    plugins: [...input.plugins], cheats: input.cheats.map(({ pack, id }) => ({ pack, id })), overlay, autostart: !input.plugins.length && input.autostart };
}
export function validateProfiles(profiles) {
  invariant(profiles && typeof profiles === 'object' && !Array.isArray(profiles) && Object.keys(profiles).length <= 256, 'PROFILE_LIMIT', 'Invalid saved loadout library.');
  const result = {};
  for (const [key, input] of Object.entries(profiles)) {
    const profile = validateProfile(input);invariant(key === profile.titleId + '@' + profile.version, 'PROFILE_IDENTITY', 'Saved loadout identity does not match its key.');result[key] = profile;
  }
  return result;
}
export class Profiles {
  constructor(store, { changed = () => {} } = {}) { this.store = store;this.changed = changed; }
  key(titleId, version) { return titleId + '@' + version; }
  async save(input, revision) {
    invariant(Number.isSafeInteger(revision), 'REVISION_REQUIRED', 'Refresh the loadout before saving.');
    const profile = validateProfile(input);
    invariant(profile.plugins.length === 0, 'FEATURE_REMOVED', 'Plugin choices are no longer supported. Save this setup without them; original files are not deleted.');
    return this.store.transaction(state => {
      const key = this.key(profile.titleId, profile.version);
      invariant(Object.hasOwn(state.profiles, key) || Object.keys(state.profiles).length < 256, 'PROFILE_LIMIT', 'The loadout library is full.');
      const before = state.profiles[key] ?? null;state.profiles[key] = profile;
      this.changed(state, key, before, profile);
    }, revision);
  }
  async remove(titleId, version, revision) {
    invariant(Number.isSafeInteger(revision), 'REVISION_REQUIRED', 'Refresh the loadout before removing it.');
    const key = this.key(titleId, version);
    return this.store.transaction(state => { invariant(Object.hasOwn(state.profiles, key), 'PROFILE_MISSING', 'Loadout not found.');
      const before = state.profiles[key];delete state.profiles[key];this.changed(state, key, before, null); }, revision);
  }
  list() { return Object.values(this.store.snapshot().profiles); }
  plan(game, catalog) {
    requireGame(game);
    const profile = this.store.snapshot().profiles[this.key(game.titleId, game.version)];
    if (!profile) return { game: clone(game), profile: null, actions: [], issues: [] };
    const actions = [], issues = [];
    if (profile.plugins.length) issues.push({ code: 'FEATURE_REMOVED', message: 'This saved setup contains retired choices. Edit and save it before applying it.' });
    for (const reference of profile.cheats) {
      const pack = catalog.cheats.find(item => item.sha256 === reference.pack);
      if (!pack || pack.titleId !== game.titleId || pack.version !== game.version || !pack.cheats.some(item => item.id === reference.id)) {
        issues.push({ code: 'CHEAT_UNAVAILABLE', ...reference }); continue;
      }
      actions.push({ kind: 'cheat.enable', ...reference, generation: game.generation });
    }
    // No partial autoload when one part of the chosen loadout is incompatible.
    return { game: clone(game), profile: clone(profile), actions, issues,
      canApply: issues.length === 0, autostart: profile.autostart && issues.length === 0 };
  }
}
