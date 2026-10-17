# Building ProsperoMenu

Toolchain: Node.js 24 or newer, Zig 0.14.1 and the
[PS5 Payload SDK v0.43](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.43).
The scripts drive Zig's Clang and LLD against the PS5 SDK, not the host libc.
Windows and Linux both work.

## Inputs

The build links pinned PS5 static libraries and checks each one against the
SHA-256 in `vendor/download/inputs.json` and `scripts/prepare-catalog.mjs`:

- `libcurl.a`, `libwolfssl.a`, `libpsl.a`, `libzstd.a`, `libz.a`: the prebuilt
  PS5 libraries in etaHEN's `Source Code/lib` folder.
- `libsqlite3.a` and `sqlite3.h`: SQLite 3.46.1 from the PS5 SDK homebrew
  package (pacbrew).

`scripts/prosperomenu/build-inputs.sh` rebuilds the same libraries from upstream
source with the PS5 SDK. Those builds are functionally equivalent but not
byte-identical, so they need the pinned hashes updated.

## Build

```sh
npm ci
export ZIG=/path/to/zig
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export PROSPEROMENU_DOWNLOAD_LIBS=/path/to/etaHEN/Source\ Code/lib
export PROSPEROMENU_SQLITE_LIB=/path/to/libsqlite3.a
export PROSPEROMENU_SQLITE_HEADER=/path/to/sqlite3.h
node scripts/prepare-components.mjs
node scripts/prepare-catalog.mjs
node scripts/prepare-download.mjs
npm run build:overlays
npm run build
node scripts/prosperomenu/package-release.mjs   # optional: release/ProsperoMenu-v<version>/
```

On Windows set the same variables with `$env:NAME = '...'` in PowerShell.

`npm run build:overlays` builds the program overlays in `sdk/examples/` into
`build/overlays/<id>/` (`overlay.json` and `<id>.ovl`); `npm run build` embeds
them so they are installed on first start.

## Outputs

| File | Use |
|---|---|
| `build/prosperomenu-bundled.elf` | The release payload (`ProsperoMenu.elf`). It always starts the runtime it carries. |
| `build/prosperomenu.elf` | Launcher that follows a saved update selection |
| `build/prosperomenu-runtime.elf` | The runtime on its own, for development |
| `build/overlays/<id>/` | Program overlays, ready to copy to `/data/ProsperoMenu/overlays/` |

`build/manifest.json` records the source identity and the hash of every
artifact.

## Third-party source

ProsperoMenu doesn't bundle kstuff, ShadowMount or any other third-party payload;
it uses the copies started by your autoloader. The only embedded component is
`prospero-bootanim` (the ShellUI MP4 launcher used for boot animations), pinned in
`vendor/components.json`. The linked libraries are pinned in
`vendor/release-sources.json` with the upstream archive URL and SHA-256 of their
source. Authorship and licence notices remain with the vendored and upstream
source.

## Firmware data

`vendor/firmware-provenance.json` is the table of firmware offsets with their
source commits. Run `node scripts/generate-firmware-profiles.mjs` after changing
it; the build fails if the generated files drift.
