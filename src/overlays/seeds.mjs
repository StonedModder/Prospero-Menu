// SPDX-License-Identifier: GPL-3.0-or-later
// Overlays written into an empty /data/ProsperoMenu/overlays on first start.
// They are ordinary overlay folders: users can edit, reorder ("order"), hide
// ("hidden": true) or delete them. The ELF examples built from sdk/examples are
// added by the build (build/overlay-seeds.json).
const json = value => JSON.stringify(value, null, 2) + '\n';
const builtin = (id, name, page, order, description, version = '1.0.0') => ({
  id, version,
  files: { 'overlay.json': json({ id, name, kind: 'builtin', page, version, author: 'ProsperoMenu', api: 1, order, description }) }
});

export const BUILTIN_SEEDS = Object.freeze([
  builtin('games', 'Games', 'games', 110, 'Play installed games and mount game sources (built-in page).', '1.0.1'),
  builtin('app-dumper', 'App Dumper', 'dumper', 115, 'Copy a mounted title to a USB drive and watch the progress (built-in page).', '1.0.1'),
  builtin('elf-updater', 'Elf-Updater', 'elf-updater', 105, 'Update kstuff, ShadowMount and zftpd from GitHub, check them by SHA-256 and arrange the startup order.'),
  {
    id: 'quick-launch', version: '1.0.1',
    files: {
      'overlay.json': json({ id: 'quick-launch', name: 'Quick Launch', kind: 'menu', menu: 'menu.json', version: '1.0.1', author: 'ProsperoMenu', api: 1, order: 200,
        description: 'Example of an overlay written in JSON only: shortcuts to other overlays and your own payloads.' }),
      'menu.json': json({
        start: 'main',
        pages: {
          main: { title: 'Quick Launch', subtitle: 'Edit menu.json in this folder', items: [
            { kind: 'header', label: 'Overlays' },
            { label: 'Fan Control', detail: 'Open the Fan Control overlay.', action: { type: 'overlay', id: 'fan-control' } },
            { label: 'Console Tools', detail: 'Open the Console Tools overlay.', action: { type: 'overlay', id: 'console-tools' } },
            { kind: 'header', label: 'Your payloads' },
            { label: 'How to add a payload', detail: 'Copy an ELF into /data/ProsperoMenu/overlays/quick-launch/ and add an item with "action": {"type": "payload", "file": "name.elf"}.', action: { type: 'page', page: 'help' } },
            { kind: 'header', label: 'Test' },
            { label: 'Send a notification', detail: 'Checks that menu actions work.', action: { type: 'notify', text: 'Quick Launch works' } }
          ] },
          help: { title: 'Adding a payload', items: [
            { label: '1. Copy the ELF here', detail: 'FTP it to /data/ProsperoMenu/overlays/quick-launch/, next to menu.json.' },
            { label: '2. Add an item', detail: '{"label": "My payload", "action": {"type": "payload", "file": "my.elf"}}' },
            { label: '3. Reopen the menu', detail: 'The menu reads menu.json each time you open this overlay. No rebuild needed.' },
            { label: 'Back', detail: 'Return to Quick Launch.', action: { type: 'page', page: 'main' } }
          ] }
        }
      })
    }
  }
]);
