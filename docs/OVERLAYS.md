# ProsperoMenu overlays

Everything in ProsperoMenu's list comes from one folder on the console:

```
/data/ProsperoMenu/
  overlays/<id>/overlay.json   one folder per overlay (you add, edit, delete these)
  run/<id>.session             written by the menu when it starts an overlay
  settings.json                ProsperoSettings (theme colours)
  seeded.json                  which bundled examples were already written
```

The menu creates these folders when they are missing. The only entry that is
not an overlay is **ProsperoSettings**, always last. The list is read again
every time the menu opens, so adding or fixing an overlay never needs a new
ProsperoMenu ELF: copy the folder over FTP (or with the file browser) and reopen
the menu. An overlay whose `overlay.json` is broken still shows up, greyed out,
with the reason as its description.

## overlay.json

```json
{
  "id": "fan-control",
  "name": "Fan Control",
  "version": "1.0.0",
  "author": "you",
  "api": 1,
  "kind": "elf",
  "elf": "fan-control.ovl",
  "order": 30,
  "keepAlive": true,
  "autostart": true,
  "description": "Shown under the list when this entry is focused."
}
```

| Field | Meaning |
|---|---|
| `id` | Must equal the folder name. 1-15 characters: `a-z 0-9 - _` |
| `name` | Text in the list (48 characters) |
| `kind` | `elf` (a program, below), `menu` (JSON only) or `builtin` (a ProsperoMenu page) |
| `order` | Sort key, lower first (default 500), then by name |
| `hidden` | `true` keeps the overlay installed but out of the list |
| `keepAlive` | ELF only: keep running when another overlay opens or the user leaves |
| `elf` | ELF only: the overlay binary in the folder, `<id>.ovl` by default (an ordinary PS5 payload ELF) |
| `autostart` | ELF only: start (hidden) once ProsperoMenu has finished starting |
| `page` | builtin only: `payloads`, `games`, `dumper`, `elf-updater`, `autoload`, `transfers`, `packages`, `files`, `console-settings` |
| `menu` | menu only: the JSON file with the pages (default `menu.json`) |

## ELF overlays

An ELF overlay is an ordinary PS5 payload. The menu starts it with its own
loader under the process name `pmo_<id>`, writes `run/<id>.session`
(`<port> <token>`), and the overlay connects to `127.0.0.1:<port>`. The
overlay's stdout and stderr go to `overlays/<id>/overlay.log`: read it over FTP
when something goes wrong. Overlay code never runs inside the system UI; the
menu validates everything it sends before drawing it.

Use the C SDK in `sdk/` (MIT licensed, `sdk/include/pmo.h`). Build the examples
with:

```powershell
node scripts/prosperomenu/build-overlays.mjs              # build/overlays/<id>/ for the console
node scripts/prosperomenu/build-overlays.mjs --host       # PC builds for testing with `npm run dev`
```

`npm run build` seeds every folder in `build/overlays/` into a fresh
`/data/ProsperoMenu/overlays/`. A seeded overlay that you delete stays deleted;
ProsperoSettings → Restore example overlays writes them again.

### Wire protocol (API 1)

One JSON object per line in each direction.

| Direction | Message | Meaning |
|---|---|---|
| overlay → menu | `{"type":"hello","id","token","api":1,"pid"}` | First line, within 5 s |
| overlay → menu | `{"type":"page","title","subtitle","footer"?,"items":[...]}` | Replace the page |
| overlay → menu | `{"type":"patch","set":{"<id>":{"value","label","detail","on","current","enabled"}}}` | Update rows |
| overlay → menu | `{"type":"notice","text"}` | System notification |
| overlay → menu | `{"type":"log","text"}` | Line in the menu's runtime log |
| overlay → menu | `{"type":"exit"}` | Return to the list |
| overlay → menu | `{"type":"pong"}` | Answer to `ping` |
| menu → overlay | `welcome`, `show`, `hide`, `back`, `quit`, `ping` | Lifecycle |
| menu → overlay | `{"type":"event","kind":"select"\|"toggle"\|"slide","id","value"}` | User input |

Items: `{"kind":"header"|"item"|"toggle"|"slider"|"text","id","label","value","detail","enabled"}`,
toggles add `on`, sliders add `min`, `max`, `step`, `current`, `unit`. At most
500 items. Labels are cut at 95 bytes, values at 31, details at 159.

Circle sends `back`. The overlay owns its page stack: answer with the previous
page, or `exit` at its first page. If the overlay does not answer within 1.5 s,
the menu returns to the list anyway.

**Watchdog.** The menu pings every second. After 3 s of silence the page says
"Overlay not responding"; after 10 s the process is stopped. The C SDK answers
pings from its own thread.

## Menu overlays (no compiler)

`"kind": "menu"` overlays are a `menu.json` of pages. See the seeded
`quick-launch` overlay. Actions: `page` (open another page), `overlay` (open
another overlay by id), `payload` (start an ELF in the same folder), `notify`.

## Example overlays

| id | What it shows | Learned from |
|---|---|---|
| `fan-control` | CPU/SoC temperatures; holds a fan threshold, re-applied every 15 s and after restarts | Elf Arsenal `src/fan.c`, OnionHEN `daemon_fan.cpp` |
| `console-tools` | Firmware, network address, uptime, temperatures, free space per drive (with a details page per drive) | Elf Arsenal `src/sensors.c` |
| `quick-launch` | JSON-only shortcuts | |
| `payloads`, `games`, `plugins`, `transfers` | ProsperoMenu's own pages | ProsperoMenu |
