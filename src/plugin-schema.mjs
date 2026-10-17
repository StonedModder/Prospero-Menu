// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
const title = /^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/;
const version = /^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/;
export function validatePlugin(item) {
  const format = item?.format ?? 'plugin', module = ['prx', 'sprx'].includes(format);
  invariant(item && /^[a-f0-9]{64}$/.test(item.sha256) && /^[a-f0-9]{32}$/.test(item.upload) &&
    ['plugin', 'prx', 'sprx'].includes(format) && typeof item.name === 'string' &&
    /^[^/\\\x00-\x1f]{1,180}\.(plugin|prx|sprx)$/i.test(item.name) && item.name.toLowerCase().endsWith('.' + format) &&
    (module ? item.pluginId === 'module:' + item.sha256 && item.pluginVersion === null :
      /^[A-Za-z]{4}\d{5}$/.test(item.pluginId) && /^\d\.\d{2}$/.test(item.pluginVersion)) &&
    ['system', 'game'].includes(item.scope) && typeof item.autostart === 'boolean', 'PLUGIN_CONFIG', 'Invalid plugin registration.');
  invariant(item.scope === 'system' ? item.target === null : item.target && title.test(item.target.titleId) && version.test(item.target.version),
    'PLUGIN_TARGET', 'Game autostart requires an exact title ID and version.');
  if (module) invariant(item.scope === 'game' && ['ps4', 'ps5'].includes(item.platform) &&
    item.platform === (/^(PPSA|PPSB|MOUU)/.test(item.target.titleId) ? 'ps5' : 'ps4') &&
    (item.platform === 'ps5' ? /^\d{2}\.\d{3}\.\d{3}$/ : /^\d{2}\.\d{2}$/).test(item.target.version),
    'PLUGIN_TARGET', 'A native module needs the exact target game, platform and version.');
  return { sha256: item.sha256, upload: item.upload, name: item.name, pluginId: item.pluginId, format,
    platform: module ? item.platform : null,
    pluginVersion: item.pluginVersion, scope: item.scope, autostart: false,
    target: item.scope === 'system' ? null : { titleId: item.target.titleId, version: item.target.version } };
}
export function validatePlugins(items) {
  invariant(Array.isArray(items) && items.length <= 64, 'PLUGIN_LIMIT', 'At most 64 plugins can be registered.');
  const result = items.map(validatePlugin);
  invariant(new Set(result.map(item => item.sha256)).size === result.length && new Set(result.map(item => item.pluginId)).size === result.length,
    'PLUGIN_CONFLICT', 'Only one registered version may own a plugin identity.');
  return result;
}
