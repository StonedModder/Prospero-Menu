// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';
import { FIRMWARES } from './firmware-profiles.mjs';
export { FIRMWARES } from './firmware-profiles.mjs';
export function requireFirmware(version) {
  invariant(FIRMWARES.includes(version), 'FIRMWARE_UNSUPPORTED',
    `ProsperoMenu has no supported profile for ${String(version).slice(0, 32)}.`,
    { action: 'Use a build with an exact firmware profile; do not substitute offsets.' });
  return version;
}
export function requireGame(game) {
  invariant(game && /^(PPSA|PPSB|MOUU|CUSA|PCAS|PCJS|PCKS|CUHJ|SCUS)\d{5}$/.test(game.titleId) &&
    /^(?:\d{2}\.\d{3}\.\d{3}|\d{2}\.\d{2})$/.test(game.version) &&
    Number.isSafeInteger(game.pid) && game.pid > 0 && typeof game.generation === 'string' &&
    game.generation.length > 0 && game.generation.length <= 128,
  'GAME_IDENTITY_REQUIRED', 'A verified game title, version and process instance are required.');
  return game;
}
