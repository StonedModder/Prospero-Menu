// SPDX-License-Identifier: GPL-3.0-or-later
import './runtime/globals.mjs';
import './runtime/url.mjs';
import { createHash, randomBytes } from './runtime/crypto.mjs';
import { StateStore } from '../src/store.mjs';
import { FileStorage } from '../src/storage.mjs';
import { PmRuntime } from '../src/runtime.mjs';
import { serve } from '../src/server.mjs';
import { PayloadInbox } from '../src/inbox.mjs';
import { installPackageInbox } from '../src/package-inbox.mjs';
import { installFileManager } from '../src/file-manager.mjs';
import { installDownloadLibrary } from '../src/download-library.mjs';
import { fileManagerIO } from './runtime/fs.mjs';
import { installLocalFiles } from '../src/local-files.mjs';
import { installPackageInstaller } from '../src/package-install.mjs';
import { installPackageReceiving } from '../src/package-receive.mjs';
import { installPackageReceiver } from '../src/package-receiver.mjs';
import { startFtp } from '../src/ftp.mjs';
import { installPayloadLoader } from '../src/payload-loader.mjs';
import { installStartupPayloads } from '../src/startup-payloads.mjs';
import { startBootAnimation, pickBootClip, BOOTANIM_FOLDER } from '../src/boot-animation.mjs';
import { installAutoload } from '../src/autoload.mjs';
import { installAppDumper } from '../src/app-dumper.mjs';
import { installElfUpdater } from '../src/elf-updater.mjs';
import { ftpInboxReceiver } from '../src/ftp-inbox.mjs';
import { ComponentAssets } from '../src/components.mjs';
import { installNativeServices } from '../src/native-services.mjs';
import { installLibrary } from '../src/library.mjs';
import { installStorageJobs } from '../src/storage-jobs.mjs';
import { installConsoleDeck } from '../src/native-deck.mjs';
import { installGameLifecycle } from '../src/game-lifecycle.mjs';
import { installCheats } from '../src/cheat-service.mjs';
import { installMemory } from '../src/memory-service.mjs';
import { installMcp } from '../src/mcp.mjs';
import { installTelemetry } from '../src/telemetry.mjs';
import { installPs4Fps } from '../src/ps4-fps.mjs';
import { installGameInput } from '../src/game-input.mjs';
import { installGameInputRelay } from '../src/game-input-relay.mjs';
import { installLoadouts } from '../src/loadouts.mjs';
import { installSaves } from '../src/saves.mjs';
import { installCheatSources } from '../src/cheat-sources.mjs';
import { installDownloads } from '../src/downloads.mjs';
import { installUpdates } from '../src/updates.mjs';
import { startupNotification } from '../src/startup-notification.mjs';
import { installOverlays } from '../src/overlays/index.mjs';
import * as fsPromises from './runtime/fs.mjs';
import overlaySeeds from '../build/overlay-seeds.json';
import brand from '../build/brand.json';
import components from '../build/components.json';
import assets from '../build/ui-assets.json';

globalThis.__ready = (async () => {
  const id = () => randomBytes(16).toString('hex');
  const store = await new StateStore(await new FileStorage('/data/ProsperoMenu/state').init(),
    value => createHash('sha256').update(value).digest('hex')).open();
  const platform = {
    operationLog: event => console.log('ProsperoMenu operation: ' + JSON.stringify(event)),
    identity: async () => ({ mode: 'console', ip: __native.ip(), firmware: __native.firmware(), hardwareVerified: false }),
    async install(runtime) {
      runtime.inbox = await new PayloadInbox('/data/ProsperoMenu/inbox', { id }).init();
      runtime.inboxItems = await runtime.inbox.list();
      await installPackageInbox(runtime, '/data/ProsperoMenu/packages');
      installLocalFiles(runtime);
      installFileManager(runtime,{io:fileManagerIO});
      installDownloadLibrary(runtime);
      installDownloads(runtime, __native);
      await installUpdates(runtime, __native);
      installPackageInstaller(runtime, __native);
      installPackageReceiving(runtime);
      installPackageReceiver(runtime);
      installSaves(runtime, __native);
      runtime.capability('inbox', true, 'Resumable file upload and verification.');
      installPayloadLoader(runtime, __native);
      installStartupPayloads(runtime, __native);
      installAutoload(runtime, { fs: fsPromises });
      installAppDumper(runtime);
      installElfUpdater(runtime, { fs: fsPromises });
      runtime.graph.register({ id: 'coordinator', start: async () => ({ session: runtime.session }),
        verify: evidence => evidence.session === runtime.session });
      await runtime.graph.start('coordinator');
      const componentAssets = new ComponentAssets(runtime.inbox, components, (offset, length) => __native.asset(offset, length));
      runtime.nativeServices = installNativeServices(runtime, __native, componentAssets);
      installLibrary(runtime, { catalog: () => __native.installedGames(), native: __native, log: message => console.log(message) });
      installStorageJobs(runtime);
      // ProsperoMenu: /data/ProsperoMenu/{overlays,run} are created here when
      // missing; the menu's root list is read from the overlays folder.
      try { await installOverlays(runtime, __native, { fs: fsPromises, seeds: overlaySeeds, logo: Buffer.from(brand.logo, 'base64'), random: n => randomBytes(n) }); }
      catch (error) { runtime.overlaysError = String(error.stack || error.message); console.error('ProsperoMenu overlays unavailable: ' + runtime.overlaysError); }
      console.log('ProsperoMenu startup: before bootanim');
      console.log('ProsperoMenu bootanim: build=guard4-spawn-retry');
      try {
        const runBootAnimation = prepared => startBootAnimation({ native: __native, components: componentAssets, prepared,
          fs: fsPromises, log: message => console.log(message), state: runtime.bootAnimation });
        const names = await fsPromises.readdir(BOOTANIM_FOLDER).catch(() => []);
        const count = names.filter(name => pickBootClip([name])).length;
        const enabled = !!store.snapshot().settings.bootAnimation?.enabled;
        runtime.bootAnimation = {
          enabled, count, playing: false,
          // Extract and verify the launcher while the deck is still attaching,
          // so the startup dispatch does not pay for it after the deck is up.
          warm: () => componentAssets.prepare('bootanim').catch(error => { console.log('ProsperoMenu startup: bootanim warm-up failed: ' + (error && error.message || error));return null; }),
          preview: async prepared => {
            const result = await runBootAnimation(prepared);
            Object.assign(runtime.bootAnimation, result);
            return result;
          }
        };
        if (enabled) {
          console.log('ProsperoMenu startup: bootanim enabled; dispatch deferred until the deck is up');
        } else console.log('ProsperoMenu startup: bootanim off');
      } catch (e) { console.log('ProsperoMenu startup: bootanim failed: ' + (e && e.message || e)); }
      console.log('ProsperoMenu startup: before deck');
      try { installConsoleDeck(runtime, __native); }
      catch (error) { console.log('ProsperoMenu startup: deck install failed: ' + String(error && error.message || error).slice(0, 200)); }
      console.log('ProsperoMenu startup: after deck install');
      installGameLifecycle(runtime, __native);
      installCheats(runtime, __native, await new FileStorage('/data/ProsperoMenu/cheat-packs').init());
      installCheatSources(runtime);
      installMemory(runtime, __native);installMcp(runtime);
      installPs4Fps(runtime, __native);installTelemetry(runtime, __native);
      // ProsperoMenu: the menu must work in games as well as on the home screen,
      // so prepare the shared game input callback automatically for each
      // verified game instance (target() still requires a ready deck renderer,
      // verified game metadata and kstuff injection readiness). the original project
      // keeps this off until the path is hardware validated.
      installGameInput(runtime, __native, { automatic: true });
      installGameInputRelay(runtime, __native);
      installLoadouts(runtime, __native);
      runtime.graph.register({ id: 'ftp', start: async () => {
        runtime.ftp = await startFtp({ root: '/data/ProsperoMenu/share', host: '0.0.0.0', address: __native.ip(), id,
          onUpload: ftpInboxReceiver(runtime) });
        return { session: runtime.session, port: runtime.ftp.port };
      }, verify: evidence => evidence.session === runtime.session && evidence.port === 2122 });
      if (runtime.store.snapshot().settings.services.ftp) {
        await runtime.graph.start('ftp'); runtime.capability('ftp', true, 'ProsperoMenu share on port 2122. Console and USB filesystem adapters remain separate.');
      }
    }
  };
  const runtime = await new PmRuntime(store, platform, { id }).start();
  await serve(runtime, { assets, host: '0.0.0.0', port: 8780, id });
  __native.notify('ProsperoMenu is starting');
  // Reserve ordered startup before automatic game consumers can enqueue work.
  console.log("ProsperoMenu startup: before configured services");
  const startup = runtime.nativeServices.startConfigured();
  console.log("ProsperoMenu startup: configured services scheduled");
  console.log("ProsperoMenu startup: before gameLifecycle");
  runtime.gameLifecycle.start();
  console.log("ProsperoMenu startup: after gameLifecycle");
  console.log("ProsperoMenu startup: before loadouts");
  runtime.loadouts?.start();
  console.log("ProsperoMenu startup: after loadouts");
  console.log("ProsperoMenu startup: before telemetry");
  runtime.telemetry?.start();
  console.log("ProsperoMenu startup: after telemetry");
  console.log("ProsperoMenu startup: before ps4Fps");
  runtime.ps4Fps?.start();
  console.log("ProsperoMenu startup: after ps4Fps");
  console.log("ProsperoMenu startup: before gameInput");
  runtime.gameInput?.start();
  console.log("ProsperoMenu startup: after gameInput");
  console.log("ProsperoMenu startup: before gameInputRelay");
  runtime.gameInputRelay?.start();
  console.log("ProsperoMenu startup: after gameInputRelay");
  // The boot animation dispatches only after the deck and services are up:
  // fullscreen playback during the deck's ShellUI widget bootstrap crashed the
  // deck (DECK_UNCERTAIN) and left the menu unopenable. A short settle keeps the
  // playback clear of the remaining native startup jobs.
  if (runtime.bootAnimation?.enabled) (async () => {
    // The boot-animation launcher detours ShellUI's Update, the same function
    // the deck hooks, and ptraces ShellUI while it injects. Started before the
    // deck (a fixed 4 s used to be enough while the menu loaded kstuff itself;
    // it now waits for the autoloader's), it made the deck fail its ShellUI
    // identity check and then refuse to hook an already-detoured Update for the
    // rest of the boot. Play only once the deck is up; never in its place.
    const warmed = runtime.bootAnimation.warm();
    const settled = () => !runtime.graph.pending.has('native-deck') && runtime.graph.states.get('native-deck')?.status !== 'starting';
    const until = Date.now() + 240000;
    while (Date.now() < until && !runtime.capabilities.get('deck')?.available && !(settled() && runtime.graph.states.get('native-deck')?.status === 'failed'))
      await new Promise(resolve => setTimeout(resolve, 100));
    if (!runtime.capabilities.get('deck')?.available) {
      console.log('ProsperoMenu startup: bootanim skipped: the menu deck is not up, and the video would block a later deck start');
      return;
    }
    const clock = () => { try { return Math.round(__native.gameInputClock()); } catch { return -1; } };
    console.log('ProsperoMenu startup: deck up at ' + clock() + ' ms; dispatching bootanim');
    try {
      const result = await runtime.bootAnimation.preview(await warmed);
      console.log('ProsperoMenu startup: bootanim ' + (result.started ? 'dispatched ' + result.clip : 'skipped: ' + result.reason) + ' (confirmed at ' + clock() + ' ms)');
    } catch (e) { console.log('ProsperoMenu startup: bootanim failed: ' + (e && e.message || e)); }
  })();
  // The phone stays responsive while durable native startup jobs execute.
  startup.then(result => {
    __native.notify(startupNotification(result, runtime.capabilities.get('deck')?.available, runtime.identity.ip));
  }).catch(error => {
    console.error(error.message); __native.notify(startupNotification({ error }, false, runtime.identity.ip));
  }).finally(() => {
    // Monitoring also adopts services the autoloader starts after the menu, so
    // it runs after a failed startup too.
    runtime.nativeServices.startMonitoring(); runtime.nativeDeck.startMonitoring();
    // Overlays that ask to start with the menu (fan control) wait for native
    // startup: their spawn shares the native-operation lock with kstuff and
    // the deck injection.
    try { runtime.overlays?.host?.startAutostart(); } catch (error) { console.error('ProsperoMenu overlays autostart: ' + error.message); }
  });
})().catch(error => { console.error(error.stack || error.message); __native.notify('ProsperoMenu could not start. Existing HEN services were not changed.'); __native.stop(); });
