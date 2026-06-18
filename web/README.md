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
