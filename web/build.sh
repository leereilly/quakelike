#!/usr/bin/env bash
#
# build.sh -- compile the software-rendered WinQuake to WebAssembly with
# Emscripten + SDL2. Run from the web/ directory (or anywhere; paths are
# resolved relative to this script).
#
# Requirements: an activated emsdk (so `emcc` is on PATH). If EMSDK_DIR is set
# (or ~/emsdk exists) this script will source its env for you.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WQ="$REPO_DIR/WinQuake"
OUT_DIR="$SCRIPT_DIR/dist"

# --- locate emcc ------------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
	EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
	if [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
		# shellcheck disable=SC1091
		source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
	fi
fi
command -v emcc >/dev/null 2>&1 || { echo "error: emcc not found on PATH"; exit 1; }

mkdir -p "$OUT_DIR"

# --- portable WinQuake C sources (software renderer, no x86 asm) -------------
WQ_SRCS=(
	chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console crc cvar
	d_edge d_fill d_init d_modech d_part d_polyse d_scan d_sky d_sprite d_surf
	d_vars d_zpoint draw host host_cmd keys mathlib menu model
	net_loop net_main net_vcr net_none
	nonintel pr_cmds pr_edict pr_exec
	r_aclip r_alias r_bsp r_draw r_edge r_efrag r_light r_main r_misc r_part
	r_sky r_sprite r_surf r_vars
	sbar screen snd_dma snd_mem snd_mix
	sv_main sv_move sv_phys sv_user
	view wad world zone cd_null
)

SRC_FILES=()
for s in "${WQ_SRCS[@]}"; do
	SRC_FILES+=("$WQ/$s.c")
done
# web platform layer
SRC_FILES+=(
	"$SCRIPT_DIR/sys_sdl.c"
	"$SCRIPT_DIR/vid_sdl.c"
	"$SCRIPT_DIR/in_sdl.c"
	"$SCRIPT_DIR/snd_sdl.c"
)

# --- compiler flags ---------------------------------------------------------
# The code base is from 1996/1997; tame modern clang so it still builds.
CFLAGS=(
	-O2
	-w
	-Wno-error=implicit-function-declaration
	-Wno-error=implicit-int
	-Wno-error=int-conversion
	-I"$WQ"
	-sUSE_SDL=2
)

LDFLAGS=(
	-sUSE_SDL=2
	-sALLOW_MEMORY_GROWTH=1
	-sINITIAL_MEMORY=64MB
	-sSTACK_SIZE=5MB
	-sFORCE_FILESYSTEM=1
	-sEXIT_RUNTIME=0
	-Wl,--allow-multiple-definition
	-lidbfs.js
	--shell-file "$SCRIPT_DIR/shell.html"
)

# --- preload game data if present ------------------------------------------
# We never commit id Software data. If web/id1/ exists (e.g. populated by CI or
# locally), preload it so /id1/pak0.pak is available in the virtual filesystem.
if [ -d "$SCRIPT_DIR/id1" ] && [ -n "$(ls -A "$SCRIPT_DIR/id1" 2>/dev/null)" ]; then
	echo "Preloading game data from web/id1/ ..."
	LDFLAGS+=(--preload-file "$SCRIPT_DIR/id1@/id1")
else
	echo "WARNING: web/id1/ is empty or missing -- the build will not be playable."
	echo "         Add id1/pak0.pak (e.g. the shareware pak) before/at deploy time."
fi

echo "Compiling ${#SRC_FILES[@]} translation units ..."
emcc "${CFLAGS[@]}" "${SRC_FILES[@]}" "${LDFLAGS[@]}" -o "$OUT_DIR/index.html"

# A .nojekyll keeps GitHub Pages from mangling the _-prefixed emscripten files.
touch "$OUT_DIR/.nojekyll"

echo "Done. Output in $OUT_DIR/"
