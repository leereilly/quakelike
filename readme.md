# quakelike

This is id Software's original Quake source code, brought back to life in the
browser. The portable C engine (`WinQuake`) is compiled to WebAssembly with
Emscripten, so the 1996 software renderer runs at full speed on a modern web
page with no plugins, no GPU shaders, and no install. On top of that base I've
added a bunch of things the original never had.

The five I'm most proud of:

1. **Quake, in your browser.** The real engine compiled to WebAssembly and
   playable on GitHub Pages. The shareware `pak0.pak` is fetched at build time,
   so the deployed site just works.
2. **Procedurally generated dungeons.** Every run can drop you into a fresh,
   walled, textured, monster-filled level, with a slipgate portal that warps you
   to a brand new layout once you clear the current one.
3. **Real-time video filters and post-process FX.** Because the renderer is
   8-bit palettized, the whole game recolors live: Red Hot, Synthwave, and
   Matrix looks, plus a CRT scanline overlay and a full ASCII-art mode that
   follows the active palette.
4. **One-click capture.** A Shot button saves a PNG and a GIF button records a
   short animated clip and encodes it right in the browser, no server round trip
   and no extra dependencies.
5. **Built to be shared.** Social share cards, deep links that restore your
   exact filter and FX combo, a death screen with your run stats, and an in-game
   toggle that streams Nine Inch Nails (Quake's original composer) while you
   play.

![quakelike](quakelike.webp)

## Play and build

The browser port lives in `web/`. See [`web/README.md`](web/README.md) for the
full story: how the SDL2 platform layer works, how to build with the Emscripten
SDK, how to run it locally over HTTP, the controls, and the GitHub Pages
deploy. The shareware data is downloaded automatically by the Pages workflow;
for a local build you drop your own `pak0.pak` into `web/id1/`.

This repo still contains the complete original sources for winquake, glquake,
quakeworld, and glquakeworld if you want to build the classic desktop versions.

## License

The code is licensed under the GNU General Public License (see `gnu.txt`). The
short version: you can do almost anything you want with the code, including sell
your own version, but if you distribute binaries you have to make the full
source available for free to everyone.

The original Quake data files are still copyrighted under id Software's original
terms, so you cannot redistribute the game data here. If you do a true total
conversion you can ship a standalone game built on this code.

Original engine by John Carmack and id Software. Release grunt work by
Dave "Zoid" Kirsh and Robert Duffy.
