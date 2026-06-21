# Copilot instructions for quakelike

id Software's original Quake source code, with a WebAssembly browser port layered
on top. The active work happens in `web/`; the rest of the tree is the unmodified
1996/1997 C sources kept for reference and desktop builds.

## Repository layout

- `WinQuake/` — portable, software-rendered single-player engine. This is the C
  code that gets compiled to WebAssembly. Do not assume it builds with modern
  warnings clean; it is 1996 C.
- `web/` — the Emscripten/WASM port. **This is where new features live.**
  - `vid_sdl.c`, `in_sdl.c`, `snd_sdl.c`, `sys_sdl.c` — SDL2 platform layer that
    replaces the original svgalib/Linux backends (video, input, audio, file I/O +
    `emscripten_set_main_loop`).
  - `procgen.c` — procedural dungeon generator (emits `func_door` BSP submodels,
    locked doors with keys, slipgate portals).
  - `shell.html` — the emcc `--shell-file` template that becomes `dist/index.html`.
    **The entire web UI and all "viral" features live here** (filters, CRT/ASCII
    toggles, GIF/screenshot capture, social share menu, music stream, touch
    controls, death-share prompt).
  - `build.sh` — the build script (see below).
- `QW/`, `qw-qc/` — QuakeWorld client/server sources and QuakeC. Reference only;
  not part of the web build.

## Build / run / test

The web port is the only thing you build here.

```bash
# requires an activated emsdk (build.sh auto-sources ~/emsdk/emsdk_env.sh)
source ~/emsdk/emsdk_env.sh
./web/build.sh              # outputs web/dist/, takes ~2-3 min
```

- `build.sh` auto-downloads the freely-redistributable **shareware** `pak0.pak`
  into `web/id1/` if missing. Set `SKIP_PAK_DOWNLOAD=1` to skip, or
  `QUAKE_PAK_URL=...` to override the source. Game data is never committed
  (id Software data is copyrighted).
- **Any change to `shell.html`, the `web/*.c` files, or `build.sh` requires a
  rebuild before it appears.** There is no dev/watch server.

Run locally (WASM must be served over HTTP, not `file://`):

```bash
cd web/dist && python3 -m http.server 8000   # open http://localhost:8000/
```

There is no unit-test suite. Verify changes by loading `dist/` in a browser, or
headless Google Chrome + puppeteer-core (args
`--use-gl=swiftshader --ignore-gpu-blocklist`) for automated checks. When you
start a local server in the background, capture its numeric PID and `kill` it
explicitly — name-based kills (`pkill`/`killall`) are forbidden in this
environment.

## Key conventions

- **JS↔engine bridge.** Anything JS needs to call in the engine is a C function
  named `Web_*` in `web/*.c`, marked `EMSCRIPTEN_KEEPALIVE`, **and** listed (with a
  leading underscore) in `build.sh`'s `-sEXPORTED_FUNCTIONS`. `shell.html` calls
  these via `Module.ccall`. If you add a `Web_*` export, you must update
  `EXPORTED_FUNCTIONS` or the symbol is stripped. `HEAP32`/`HEAPU8` are exported
  for reading buffers (e.g. the ASCII grid, procgen map data).
- **Live recoloring is palette remapping.** The renderer is 8-bit palettized, so
  video filters (`vid_filter` 0–3) and ASCII mode just remap the 256-color table
  in `VID_BuildFilter`/`VID_BuildAscii` — no GPU shaders.
- **CRT is pure CSS.** The CRT effect is a CSS overlay (`#crt-overlay`) in
  `shell.html`, not engine code. ASCII mode samples the engine grid each frame and
  draws glyphs on a `<canvas>` overlay.
- **Shareable state** (filter + CRT + ASCII) is encoded in the URL hash
  (e.g. `#f=2&crt=1`) and restored on load.
- **shell.html UI.** Toggle controls use the `.qtoggle` pill switch (not `.qbtn`).
  `.panel-grid` must use `grid-template-columns: repeat(2, minmax(0, 1fr))` — plain
  `1fr 1fr` overflows the narrow panel.
- **Networking and CD audio are stubbed** in the web build (`net_none.c`,
  `cd_null.c`); single-player only, fixed 320×200 internal resolution.

## Docs / style

- `web/README.md` is the authoritative deep-dive on the port (platform layer,
  filters, FX, capture, music, deploy). Keep it in sync when you change those
  features.
- The root `readme.md` is human-voiced prose: **no em-dashes**, avoid
  marketing/AI-tell phrasing.
- Deploy is automatic via `.github/workflows/pages.yml` on push to `master`/`main`
  (paths `WinQuake/**`, `web/**`, the workflow itself); it fetches the shareware
  pak and publishes `web/dist`.
