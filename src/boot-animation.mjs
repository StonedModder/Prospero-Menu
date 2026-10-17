// SPDX-License-Identifier: GPL-3.0-or-later
// Boot animation: play one MP4 fullscreen through ShellUI when the menu starts.
// The playback itself lives in the bundled prospero-bootanim.elf launcher
// (ShellUI resident + Sony's InitialBootPlayer); this module chooses the clip
// and hands the launcher to the console's ELF loader.
export const BOOTANIM_FOLDER = '/data/ProsperoMenu/autoloadAnims';
export const BOOTANIM_PATH_FILE = '/data/ProsperoMenu/bootanim-path.txt';
export const BOOTANIM_COMPONENT = 'bootanim';
const SAFE_CLIP_NAME = /^[A-Za-z0-9._-]+\.mp4$/;
let dispatchCounter = 0; // per-process, part of the unique dispatch identity
// The player resolves the handed-off clip as a URL: a name with spaces or
// brackets ("X - Y [1m02s-1m30s].mp4") silently falls back to a default
// animation. Sanitize unsafe names on disk once (no copies, no extra space).
export async function sanitizeBootClips(fs) {
  let names;
  try { names = await fs.readdir(BOOTANIM_FOLDER); } catch { return []; }
  const renamed = [], taken = new Set(names);
  for (const name of names) {
    if (name.startsWith('.') || !name.toLowerCase().endsWith('.mp4') || SAFE_CLIP_NAME.test(name)) continue;
    let safe = name.replace(/[^A-Za-z0-9._-]+/g, '_').replace(/^[._]+/, '').replace(/_+\.mp4$/i, '.mp4');
    if (!/\.mp4$/i.test(safe)) safe = safe.slice(0, Math.max(0, safe.lastIndexOf('.'))) + '.mp4';
    if (!/^[A-Za-z0-9]/.test(safe)) safe = 'clip-' + safe;
    let target = safe, n = 1;
    while (taken.has(target)) target = safe.slice(0, -4) + '-' + (n++) + '.mp4';
    taken.add(target);
    await fs.rename(BOOTANIM_FOLDER + '/' + name, BOOTANIM_FOLDER + '/' + target);
    renamed.push({ from: name, to: target });
  }
  return renamed;
}

// MP4s only (the player route is MP4/H.264+AAC). Hidden files are ignored.
export function pickBootClip(names, random = null) {
  const clips = (names || []).filter(name => typeof name === 'string' && !name.startsWith('.') &&
    name.toLowerCase().endsWith('.mp4'));
  if (!clips.length) return null;
  // Math.random is coarsely seeded in the console runtime: every boot drew the
  // same value and always picked the same clip. Mix the clock in per call.
  const draw = random ? random() : (Math.random() + (Date.now() % 1000003) / 1000003) % 1;
  const at = Math.min(clips.length - 1, Math.floor(draw * clips.length));
  return clips[at];
}

export function describeBootAnimation(enabled, count, folder = BOOTANIM_FOLDER) {
  if (!enabled) return `Off · place MP4 videos in ${folder}`;
  if (!count) return `On · no MP4 in ${folder}`;
  return `On · ${folder} · ${count} video${count === 1 ? '' : 's'}, one chosen at random`;
}

// The launcher mirrors its play phase into the menu's own workspace (the menu's
// fs policy only reads /data and friends, never /system_tmp). Format:
// "<token> <phase>" — rewritten whenever the phase changes.
const STATUS_MIRROR_FILE = '/data/ProsperoMenu/bootanim-status.txt';
function watchPlaybackEnd(fs, state, onPlaybackEnd, log) {
  if (state.watcher) return;
  state.watchStartedAt = Date.now();
  const timer = setInterval(async () => {
    if (!state.playing) return;
    try {
      let text = '';
      try { text = String(await fs.readFile(STATUS_MIRROR_FILE, 'utf8') || ''); } catch { text = ''; }
      const [, phase = ''] = text.trim().split(/\s+/);
      if (!phase) {
        // No launcher run materialized (a spawn can be silently dropped): do not
        // hold the button hostage — re-enable after a short grace period.
        if (Date.now() - (state.watchStartedAt || 0) > 15000) {
          state.playing = false;
          log('boot animation: no launcher run appeared; button re-enabled');
          try { onPlaybackEnd('not-started'); } catch { /* best-effort */ }
        }
        return;
      }
      // The mirror carries "<token> <phase> <rc>" or "<token> <phase>"; only the
      // phase word is the key.
      if (['done', 'failed', 'cancelled', 'failed_cleanup'].includes(phase)) {
        state.playing = false;
        log('boot animation: playback ended (' + phase + ')');
        try { onPlaybackEnd(phase); } catch { /* the button state is best-effort */ }
      } else if (Date.now() - (state.watchStartedAt || 0) > 300000) {
        // Safety valve: a play longer than five minutes is not this feature's
        // use case; never hold the button hostage to a missed status line.
        state.playing = false;
        log('boot animation: no terminal status within 5 minutes; button re-enabled');
        try { onPlaybackEnd('timeout'); } catch { /* best-effort */ }
      }
    } catch { /* best-effort */ }
  }, 2000);
  state.watcher = timer;
  if (timer && typeof timer.unref === 'function') timer.unref();
}

// Pick a clip and dispatch the launcher through the console's own loader.
// Everything is best-effort and reported; a failure never stops menu startup.
export async function startBootAnimation({ native, inbox, components, fs, log = () => {}, onPlaybackEnd = () => {}, state = null, prepared: warmed = null }) {
  // One playback at a time: the button must not be spammable while a clip runs,
  // and the launcher's own lock rejects concurrent launches anyway. The shared
  // state flips back when the launcher's status file reports the play's end.
  state = state || { playing: false };
  if (state.playing) return { started: false, reason: 'a video is already playing; try again when it finishes' };
  const renamed = await sanitizeBootClips(fs);
  for (const entry of renamed) log('boot animation: renamed ' + entry.from + ' -> ' + entry.to);
  let names;
  try { names = await fs.readdir(BOOTANIM_FOLDER); }
  catch (error) { return { started: false, reason: 'no folder: ' + String(error && error.code || error) }; }
  const clip = pickBootClip(names);
  if (!clip) return { started: false, reason: 'no MP4 in ' + BOOTANIM_FOLDER };
  try {
    await fs.writeFile(BOOTANIM_PATH_FILE, BOOTANIM_FOLDER + '/' + clip + '\n');
  } catch (error) { return { started: false, reason: 'cannot write clip selection: ' + String(error && error.message || error) }; }
  try {
    // A launcher verified moments ago at startup is reused; Preview prepares fresh.
    const prepared = warmed?.id && warmed?.sha256 ? warmed : await components.prepare(BOOTANIM_COMPONENT);
    // Clear any previous play's mirrored status so the watcher cannot mistake a
    // stale "done" line for this run's outcome.
    if (fs.unlink) await fs.unlink(STATUS_MIRROR_FILE).catch(() => {});
    // The native dispatcher validates the request identity as 32 lowercase hex
    // characters (loader-worker.c start()) AND keeps receipts on disk across
    // boots: a Math.random id repeats every boot and collides (EEXIST). Build
    // the identity from the clock and a counter so it is unique every call.
    for (let attempt = 0; attempt < 2; attempt += 1) {
      const base = (Date.now().toString(16) + (dispatchCounter++) + Math.floor(Math.random() * 0xffffffff).toString(16)).replace(/[^0-9a-f]/g, '');
      const request = (base + '00000000000000000000000000000000').slice(0, 32);
      let dispatched = false;
      for (let tryStart = 0; tryStart < 40 && !dispatched; tryStart += 1) {
        try { native.loaderStart(request, prepared.id, prepared.sha256); dispatched = true; }
        catch (error) {
          if (error?.code !== 'LOADER_BUSY') throw error; // another dispatch is running
          await new Promise(resolve => setTimeout(resolve, 100));
        }
      }
      if (!dispatched) return { started: false, reason: 'the native loader stayed busy' };
      let spawned = false;
      for (let waited = 0; waited < 240; waited += 1) {
        await new Promise(resolve => setTimeout(resolve, 250));
        const receipt = native.loaderPoll(request);
        if (receipt?.stage === 3) { spawned = true; break; }
        if (receipt?.stage === 4 || receipt?.stage === 5)
          return { started: false, reason: receipt.message || 'launcher dispatch failed' };
      }
      if (!spawned) return { started: false, reason: 'launcher dispatch timed out' };
      // The dispatcher reports stage 3 for the spawn even when the payload is
      // silently dropped (its mirror never appears). Confirm the launcher is
      // actually alive and retry once before believing the dispatch.
      if (fs.readFile) {
        let alive = false;
        for (let check = 0; check < 8 && !alive; check += 1) {
          await new Promise(resolve => setTimeout(resolve, 1000));
          try { alive = String(await fs.readFile(STATUS_MIRROR_FILE, 'utf8') || '').trim().length > 0; } catch { alive = false; }
        }
        if (!alive) {
          if (attempt === 0) { log('boot animation: dispatched launcher never started; retrying'); continue; }
          return { started: false, reason: 'the launcher never started (dispatch dropped twice)' };
        }
      }
      state.playing = true;
      watchPlaybackEnd(fs, state, onPlaybackEnd, log);
      log('boot animation dispatched: ' + clip);
      return { started: true, clip };
    }
    return { started: false, reason: 'the launcher never started' };
  } catch (error) {
    return { started: false, reason: String(error && error.message || error) };
  }
}
