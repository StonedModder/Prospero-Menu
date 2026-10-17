// SPDX-License-Identifier: GPL-3.0-or-later
// Wires the overlay registry, host and ProsperoSettings into the coordinator.
import { OverlayRegistry, PM_ROOT } from './registry.mjs';
import { OverlayHost } from './host.mjs';
import { ProsperoSettings } from './settings.mjs';
import { BUILTIN_SEEDS } from './seeds.mjs';

export async function installOverlays(runtime, native, { fs, seeds = [], logo = null, root = PM_ROOT, port, random = randomFallback, log = message => console.log(message) } = {}) {
  const registry = new OverlayRegistry({ fs, root, seeds: [...BUILTIN_SEEDS, ...seeds], log });
  // The header logo is a plain file the user replaces (any of these names).
  // Detection runs with the scan, so the frame can read it synchronously.
  const LOGO_NAMES = ['logo.png', 'logo.webp', 'logo.jpg', 'logo.jpeg', 'theme/logo.png'];
  const findLogo = async () => {
    for (const name of LOGO_NAMES) {
      try { const info = await fs.stat(root + '/' + name); if (info.isFile()) return root + '/' + name; } catch { /* keep looking */ }
    }
    return '';
  };
  const settings = new ProsperoSettings({ fs, file: root + '/settings.json', log });
  const notify = text => { try { native?.notify?.(String(text).slice(0, 200)); } catch { /* notifications are best effort */ } };
  await registry.ensure();
  // The bundled logo is written once, when no logo exists yet. Replacing the
  // file changes the logo; deleting it brings back the drawn mark.
  if (logo?.length) {
    const markerFile = root + '/seeded.json';
    let marker = {}; try { marker = JSON.parse(await fs.readFile(markerFile, 'utf8')); } catch { /* first start */ }
    if (!marker.$logo && !await findLogo()) {
      try { await fs.writeFile(root + '/logo.png', logo); marker.$logo = 1; await fs.writeFile(markerFile, JSON.stringify(marker, null, 1)); }
      catch (error) { log('ProsperoMenu: could not write the default logo: ' + error.message); }
    }
  }
  await registry.scan(); registry.logo = await findLogo(); await settings.load();
  const host = new OverlayHost({ registry, fs, native, notify, log, random, services: overlayServices(), ...(port != null ? { port } : {}) });
  let scanning = null;
  runtime.overlays = {
    registry, settings, host, fs, notify,
    rescan() { return scanning ??= registry.scan().then(async () => { registry.logo = await findLogo(); })
      .finally(() => { scanning = null; host.bump(); }); },
    async restoreExamples() { await registry.seed(true); await this.rescan(); },
    async runPayload(overlay, file) {
      if (!native?.overlaySpawn) throw new Error('Payload launching needs the console.');
      native.overlaySpawn(overlay.id, `${overlay.folder}/${file}`); notify(`${overlay.name}: started ${file}`);
    }
  };
  // Not a ProsperoMenu capability (that list is fixed); /api/debug reports this.
  runtime.overlays.status = () => ({ listening: Boolean(host.server?.listening), port: host.port, error: hostError,
    dir: registry.dir, overlays: registry.overlays.map(o => o.id), broken: registry.broken, sessions: host.snapshot() });
  let hostError = null;
  try { await host.start(); log(`ProsperoMenu: overlay host on 127.0.0.1:${host.port}, ${registry.overlays.length} overlays`); }
  catch (error) { hostError = String(error.message || error); log('ProsperoMenu: overlay host could not listen: ' + hostError); }
  return runtime.overlays;
}
// Requests overlays may make of the menu ({"type":"request"}). None are
// defined yet; the mechanism and its permission check live in host.mjs.
export function overlayServices() { return {}; }
function randomFallback(n) { const out = new Uint8Array(n); for (let i = 0; i < n; i++) out[i] = Math.floor(Math.random() * 256); return out; }
