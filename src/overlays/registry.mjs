// SPDX-License-Identifier: GPL-3.0-or-later
// Overlay discovery: /data/ProsperoMenu/overlays/<id>/overlay.json.
import { parseManifest, sortOverlays, compareVersions, OVERLAY_ID } from './manifest.mjs';

export const PM_ROOT = '/data/ProsperoMenu';

// Seeds are the example overlays bundled with this build. A seed is written
// once: deleting its folder keeps it deleted (ProsperoSettings can restore the
// examples). A seed that this build ships in a newer version replaces an older
// copy that was itself seeded and still carries the seeded version.
export class OverlayRegistry {
  constructor({ fs, root = PM_ROOT, seeds = [], log = () => {} }) {
    this.fs = fs; this.root = root; this.dir = root + '/overlays'; this.run = root + '/run';
    this.seeds = seeds; this.log = log; this.overlays = []; this.broken = []; this.scannedAt = 0;
  }
  async ensure() {
    // "make the folder directories if not found"
    for (const dir of [this.root, this.dir, this.run]) await this.fs.mkdir(dir, { recursive: true });
    await this.seed(false);
    return this;
  }
  async readJson(file, fallback) {
    try { return JSON.parse(await this.fs.readFile(file, 'utf8')); } catch { return fallback; }
  }
  async exists(file) { try { await this.fs.stat(file); return true; } catch { return false; } }
  async writeSeed(seed) {
    const folder = this.dir + '/' + seed.id;
    await this.fs.mkdir(folder, { recursive: true });
    for (const [name, data] of Object.entries(seed.files)) {
      const bytes = typeof data === 'string' ? data : Buffer.from(data.base64, 'base64');
      await this.fs.writeFile(folder + '/' + name, bytes);
    }
  }
  async seed(force) {
    const markerFile = this.root + '/seeded.json';
    const marker = await this.readJson(markerFile, {});
    let changed = false;
    for (const seed of this.seeds) {
      if (!OVERLAY_ID.test(seed.id)) continue;
      const folder = this.dir + '/' + seed.id, present = await this.exists(folder + '/overlay.json');
      const installed = present ? await this.readJson(folder + '/overlay.json', {}) : null;
      const upgrade = present && marker[seed.id] && installed?.version === marker[seed.id] && compareVersions(seed.version, marker[seed.id]) > 0;
      if (force || (!present && !marker[seed.id]) || upgrade) {
        try { await this.writeSeed(seed); marker[seed.id] = seed.version; changed = true; this.log(`ProsperoMenu: seeded overlay ${seed.id} ${seed.version}`); }
        catch (error) { this.log(`ProsperoMenu: could not seed overlay ${seed.id}: ${error.message}`); }
      }
    }
    if (changed) await this.fs.writeFile(markerFile, JSON.stringify(marker, null, 1));
  }
  async scan() {
    const overlays = [], broken = [];
    let entries = [];
    try { entries = await this.fs.readdir(this.dir, { withFileTypes: true }); }
    catch (error) { broken.push({ id: 'overlays', error: 'Overlay folder unreadable: ' + error.message }); }
    for (const entry of entries) {
      const name = typeof entry === 'string' ? entry : entry.name;
      if (name.startsWith('.') || (typeof entry !== 'string' && !entry.isDirectory())) continue;
      try {
        const text = await this.fs.readFile(this.dir + '/' + name + '/overlay.json', 'utf8');
        const overlay = parseManifest(name, text);
        if (overlay.kind === 'elf' && !await this.exists(this.dir + '/' + name + '/' + overlay.elf))
          throw Object.assign(new Error(`${overlay.elf} is missing from the overlay folder.`), { overlay });
        overlays.push({ ...overlay, folder: this.dir + '/' + name });
      } catch (error) {
        if (error.code === 'OVERLAY_RETIRED') continue;
        broken.push({ id: name, name: error.overlay?.name || name, error: error.code === 'ENOENT' ? 'No overlay.json in this folder.' : error.message });
      }
    }
    this.overlays = sortOverlays(overlays); this.broken = broken.sort((a, b) => a.id.localeCompare(b.id)); this.scannedAt = Date.now();
    return { overlays: this.overlays, broken: this.broken };
  }
  get(id) { return this.overlays.find(o => o.id === id) || null; }
}
