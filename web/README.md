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
repository** — id Software's data is copyrighted and not redistributable here.

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

## Video filters

Because the software renderer is fully palettized, the whole game can be
recolored in real time by remapping the 256-color table — no GPU shaders
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

- **📺 CRT** — scanlines + a corner vignette + subtle flicker. Pure CSS overlay
  on the canvas (`#crt-overlay` in `web/shell.html`), so it costs nothing.
- **▣ ASCII** — the engine downsamples each frame into a 100×56 grid of the
  *current* (post-filter) colors (`VID_BuildAscii` in `web/vid_sdl.c`, exported
  via `Web_GetAscii`/`Web_AsciiEnable`); the shell reads that grid every frame
  and draws glyphs over the canvas on a `<canvas>` overlay. Because it samples
  the filtered colors, the ASCII view follows the active theme — green for
  Matrix, red/orange for Red Hot, magenta/cyan for Synthwave, full color for
  Normal.

Filters and FX stack — e.g. Red Hot + CRT.

## Stream music (dark industrial, in-game)

Quake's original soundtrack was composed by **Trent Reznor / Nine Inch Nails**,
so dark industrial is the canonical vibe. The web port adds a **Stream Music**
toggle to the **in-game Options menu** (ESC → Options): it streams audio from a
hidden, off-screen SoundCloud player — **no SoundCloud UI is shown in the game**,
only the audio plays, and it keeps playing during gameplay and in fullscreen.

It works by replacing the (unavailable) "Video Options" slot in the web build's
Options menu with a checkbox. Toggling it calls a C→JS bridge
(`Web_ToggleMusic` / `Web_MusicState` in `web/snd_sdl.c`) that creates or removes
a hidden audio-only iframe.

The default source is **Nine Inch Nails** (`soundcloud.com/nineinchnails`). The
embed only resolves real SoundCloud resources — a **track, playlist, or user**
permalink — *not* `/tags/` URLs. To stream something else, change the URL inside
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

## Notes / limitations

- Software renderer only, fixed 320×200 internal resolution, scaled up by the
  browser (the authentic 1996 look). Pass `-width`/`-height` (≤ 320×200) or
  `-scale N` via the Emscripten `arguments` if you want to tweak it.
- Single-player only — networking is stubbed to the loopback driver
  (`net_none.c`), so there are no online/LAN servers.
- CD audio is disabled (`cd_null.c`); in-game sound effects work.
