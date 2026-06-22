# Quake on the Web (WebAssembly)

This directory contains an [Emscripten](https://emscripten.org/) port of the
**software-rendered** single-player Quake (`WinQuake`), playable in a browser and
deployable to GitHub Pages.

It works by compiling the original, portable C engine to WebAssembly and adding a
small SDL2-based platform layer:

| File         | Replaces            | Responsibility                                  |
|--------------|---------------------|-------------------------------------------------|
| `vid_sdl.c`  | `vid_svgalib.c`     | 8-bit palettized framebuffer → ARGB SDL texture |
| `in_sdl.c`   | (svgalib input)     | Keyboard + mouse via SDL events                 |
| `snd_sdl.c`  | `snd_linux.c`       | Audio output (SDL DMA emulation, S16→F32)       |
| `sys_sdl.c`  | `sys_linux.c`       | File I/O + `emscripten_set_main_loop` host loop |

No OpenGL is required: on WebAssembly the x86 assembly inner loops
(`id386`) are automatically disabled, so the pure-C software rasterizer is used.

## Game data (important)

The engine needs Quake game data (`id1/pak0.pak`). **It is not committed to this
repository**: id Software's data is copyrighted and not redistributable here.

- The **GitHub Pages workflow downloads the freely-available shareware
  `pak0.pak` at build time**, so the deployed site is playable out of the box.
- For local builds, drop a `pak0.pak` into `web/id1/` before building. The
  shareware pak gives you the first episode; a registered/Steam/GOG `pak0.pak`
  (plus `pak1.pak`) unlocks the full game.

```
web/
  id1/
    pak0.pak     <-- you provide this (git-ignored)
```

## Building locally

You need the Emscripten SDK on your `PATH` (the build script will auto-source
`~/emsdk/emsdk_env.sh` if present):

```bash
# one-time: install emsdk
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest

# build (outputs to web/dist/)
./web/build.sh
```

## Running locally

WebAssembly must be served over HTTP (not `file://`):

```bash
cd web/dist
python3 -m http.server 8000
# open http://localhost:8000/
```

## Controls

- **Click the canvas** to capture the mouse (pointer lock).
- Mouse to look, **WASD / arrows** to move, **Ctrl/Mouse1** to fire.
- **ESC** for the menu, **`** (backtick) for the console.
- **Fullscreen**: click the **Fullscreen** button under the canvas, or
  double-click the canvas. Press **ESC** (or F11) to exit.
- **Touch devices**: on-screen controls appear automatically, a left d-pad
  (forward/back/strafe), **drag anywhere on the view to look**, and **Fire /
  Jump / ⏎ / ESC** buttons. Tap **Play** (or the idle overlay) to drop straight
  into a level. Wired to the engine via `Web_KeyEvent` / `Web_LookDelta` (see
  `web/in_sdl.c`).

## Share & social (30th-anniversary build)

The web port is tuned to be shared:

- **Link-unfurl cards**: OpenGraph + Twitter Card meta tags in `shell.html`
  give a `summary_large_image` preview on HN/Reddit/X/Discord/Slack. The card is
  `web/og-image.gif`; `build.sh` copies it into `dist/` and the tags
  point at `https://leereilly.net/quakelike/og-image.gif`.
- **Social share menu**: the Share button opens a menu with one-click intents
  for **X, Bluesky, Reddit, Hacker News, LinkedIn, Facebook** plus Copy link,
  each prefilled with `#Quake30`.
- **Shareable look**: the active **filter + CRT + ASCII** state is encoded in
  the URL hash (e.g. `#f=2&crt=1`) and restored on load, so you can share
  "Quake in Synthwave + CRT" as a link.
- **Anniversary banner**: a dismissible "Quake turns 30 today" banner with a
  one-shot confetti burst (state stored in `localStorage`).
- **Death share prompt**: when you die, a prompt appears above the protip with
  your run stats (baddies fragged + time survived, plus the procgen seed if it
  was a generated dungeon) and a **Share your run** button into the social menu.


## Video filters

Because the software renderer is fully palettized, the whole game can be
recolored in real time by remapping the 256-color table, no GPU shaders
required. Use the buttons under the canvas, or the console command `vidfilter`
(cycles), or set the archived cvar `vid_filter` directly:

| `vid_filter` | Look |
|---|---|
| `0` | Normal |
| `1` | 🔥 Red Hot (thermal / predator vision) |
| `2` | 🌆 Synthwave (neon magenta → cyan) |
| `3` | 🟩 Matrix (green phosphor) |

The filters are implemented in `web/vid_sdl.c` (`VID_BuildFilter`). The web UI
calls the exported `Web_SetFilter()` via `Module.ccall`.

## Post-process FX

Two extra screen effects toggle from the **FX** buttons under the canvas:

- **📺 CRT**: scanlines + a corner vignette + subtle flicker. Pure CSS overlay
  on the canvas (`#crt-overlay` in `web/shell.html`), so it costs nothing.
- **▣ ASCII**: the engine downsamples each frame into a 100×56 grid of the
  *current* (post-filter) colors (`VID_BuildAscii` in `web/vid_sdl.c`, exported
  via `Web_GetAscii`/`Web_AsciiEnable`); the shell reads that grid every frame
  and draws glyphs over the canvas on a `<canvas>` overlay. Because it samples
  the filtered colors, the ASCII view follows the active theme, green for
  Matrix, red/orange for Red Hot, magenta/cyan for Synthwave, full color for
  Normal.

Filters and FX stack, e.g. Red Hot + CRT.

## Capture & share (Shot / GIF)

Two buttons under the canvas turn the live game into shareable media, perfect
for demos and social clips:

- **📷 Shot**: saves a PNG screenshot of the current frame.
- **🎞 GIF**: click to start recording, click again to stop (auto-stops after
  8 seconds), and an animated GIF downloads automatically. A small toast
  confirms each save.

Both capture **whatever you currently see**, including the active filter, the
ASCII view, and the CRT overlay (scanlines + vignette are re-drawn into the
exported pixels). GIFs are downscaled to ≤320px wide and recorded at 12 fps to
keep files small and instantly shareable.

It's implemented entirely in `web/shell.html` with no extra dependencies or
build step:

- Frames are pulled off the WebGL canvas via `canvas.captureStream()`, which
  works regardless of `preserveDrawingBuffer` (where `canvas.toDataURL()` would
  return blank).
- A compact, self-contained **GIF89a encoder** (median-cut palette + LZW)
  produces a looping animated GIF in the browser. Because Quake's software
  renderer is 8-bit palettized, a single 256-color global palette reproduces the
  clip essentially loss-free.

## Stream music (dark industrial, in-game)

Quake's original soundtrack was composed by **Trent Reznor / Nine Inch Nails**,
so dark industrial is the canonical vibe. The web port adds a **Stream Music**
toggle to the **in-game Options menu** (ESC → Options): it streams audio from a
hidden, off-screen SoundCloud player, **no SoundCloud UI is shown in the game**,
only the audio plays, and it keeps playing during gameplay and in fullscreen.

It works by replacing the (unavailable) "Video Options" slot in the web build's
Options menu with a checkbox. Toggling it calls a C→JS bridge
(`Web_ToggleMusic` / `Web_MusicState` in `web/snd_sdl.c`) that creates or removes
a hidden audio-only iframe.

The default source is **Nine Inch Nails** (`soundcloud.com/nineinchnails`). The
embed only resolves real SoundCloud resources, a **track, playlist, or user**
permalink, *not* `/tags/` URLs. To stream something else, change the URL inside
the `Web_MusicStreamJS` `EM_JS` block in `web/snd_sdl.c`.

> Browsers block autoplay until you interact with the page; toggling the menu
> item counts as that interaction, so playback starts on toggle.

The Options menu's volume slider (formerly "CD Music Volume", now **"Music
Volume"**) drives the stream volume via the SoundCloud Widget API
(`Web_SetMusicVolume` in `web/snd_sdl.c`).

## Deploying to GitHub Pages

The workflow at `.github/workflows/pages.yml` builds and deploys automatically.
To enable it:

1. In your repo, go to **Settings → Pages** and set **Source** to
   **GitHub Actions**.
2. Push to `master`/`main` (or run the workflow manually via
   **Actions → Deploy WebAssembly Quake to GitHub Pages → Run workflow**).

The site is published at `https://<owner>.github.io/<repo>/`.

## Procedural dungeons

Levels can be generated on the fly by `web/procgen.c`. Type **`procgen`** in the
console to roll a fresh dungeon (`procgen <seed>` to reproduce a specific one), or
clear the current level and step through the **slipgate** that opens to warp into
the next one.

How it works:

- **Maze layout** (`PG_Layout`): an interleaved grid of room and wall cells is
  laid down (each room gets a randomized, 32-aligned footprint), then a seeded
  random walk starts in the center and carves the wall cell between each pair of
  rooms it steps across into a connecting passage. Everything it never visits
  stays solid rock. Grids run 6–9 rooms per axis (`PG_ROOMS_MIN`/`MAX`).
- **Real BSP, not a tilemap**: the open floor plan is compiled into a genuine
  **binary space partitioning** tree (`PG_RenderTree` / `PG_BuildDungeon`),
  emitting actual planes, nodes, faces, and clipnodes that the original 1996
  engine loads exactly like a hand-built `maps/procgen.bsp`.
- **Gameplay furniture**: a turbulent **slipgate** portal, key-locked
  `func_door` brush submodels (silver/gold keys placed in spawn-reachable
  rooms), monsters, and items are populated before the map is written.
- **Deterministic & shareable**: an LCG seeds the whole process, so a given seed
  always rebuilds the same dungeon. The original seed is kept in `pg_last_seed`
  and exposed to the web UI via `Web_ProcgenSeed` (it rides along in the
  death-share prompt). `Web_ProcgenMap` returns a minimap buffer via `HEAP32`.
- **Download Map**: while inside a procgen dungeon, the **Download Map** button
  in the dungeon panel reads `id1/maps/procgen.bsp` from Emscripten's virtual
  filesystem and saves it as `quakelike-<seed>.bsp`. Drop the file into a Quake
  installation's `id1/maps/` directory and run `map quakelike-<seed>` from the
  console to explore it offline.

## Notes / limitations

- Software renderer only, fixed 320×200 internal resolution, scaled up by the
  browser (the authentic 1996 look). Pass `-width`/`-height` (≤ 320×200) or
  `-scale N` via the Emscripten `arguments` if you want to tweak it.
- Single-player only, networking is stubbed to the loopback driver
  (`net_none.c`), so there are no online/LAN servers.
- CD audio is disabled (`cd_null.c`); in-game sound effects work.
