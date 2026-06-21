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
	"$SCRIPT_DIR/procgen.c"
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
	-sEXPORTED_FUNCTIONS=_main,_Web_SetFilter,_Web_AsciiEnable,_Web_GetAscii,_Web_AsciiCols,_Web_AsciiRows,_Web_KeyEvent,_Web_LookDelta,_Web_Command,_Web_LevelTime,_Web_Intermission,_Web_MapName,_Web_IsPlaying,_Web_Health,_Web_Kills,_Web_IsProcgen,_Web_ProcgenSeed,_Web_ProcgenMap,_Web_PlayerX,_Web_PlayerY,_Web_PlayerYaw
	-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,HEAP32,UTF8ToString
	-Wl,--allow-multiple-definition
	-lidbfs.js
	--shell-file "$SCRIPT_DIR/shell.html"
)

# --- fetch shareware game data if missing ----------------------------------
# We never commit id Software data. If web/id1/pak0.pak is absent, download the
# freely-redistributable shareware pak so local builds are playable out of the
# box. Set QUAKE_PAK_URL to override the source, or SKIP_PAK_DOWNLOAD=1 to skip.
PAK_URL="${QUAKE_PAK_URL:-https://archive.org/download/quake-shareware-pak/PAK0.PAK}"
if [ ! -f "$SCRIPT_DIR/id1/pak0.pak" ] && [ "${SKIP_PAK_DOWNLOAD:-0}" != "1" ]; then
	echo "web/id1/pak0.pak not found -- downloading shareware pak ..."
	mkdir -p "$SCRIPT_DIR/id1"
	if command -v curl >/dev/null 2>&1; then
		curl -fSL --retry 3 -o "$SCRIPT_DIR/id1/pak0.pak" "$PAK_URL"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$SCRIPT_DIR/id1/pak0.pak" "$PAK_URL"
	else
		echo "error: neither curl nor wget found; cannot download pak0.pak" >&2
		exit 1
	fi
	# A valid Quake pak begins with the magic bytes "PACK".
	if [ "$(head -c 4 "$SCRIPT_DIR/id1/pak0.pak")" != "PACK" ]; then
		echo "error: downloaded pak0.pak is not a valid PACK file" >&2
		rm -f "$SCRIPT_DIR/id1/pak0.pak"
		exit 1
	fi
	echo "Downloaded shareware pak0.pak to web/id1/."
fi

# --- preload game data if present ------------------------------------------
# If web/id1/ exists (downloaded above, or populated by CI/locally), preload it
# so /id1/pak0.pak is available in the virtual filesystem.
if [ -d "$SCRIPT_DIR/id1" ] && [ -n "$(ls -A "$SCRIPT_DIR/id1" 2>/dev/null)" ]; then
	echo "Preloading game data from web/id1/ ..."
	LDFLAGS+=(--preload-file "$SCRIPT_DIR/id1@/id1")
else
	echo "WARNING: web/id1/ is empty or missing -- the build will not be playable."
	echo "         Set SKIP_PAK_DOWNLOAD=0 (the default) to auto-fetch the shareware"
	echo "         pak, or add id1/pak0.pak yourself before/at deploy time."
fi

echo "Compiling ${#SRC_FILES[@]} translation units ..."
emcc "${CFLAGS[@]}" "${SRC_FILES[@]}" "${LDFLAGS[@]}" -o "$OUT_DIR/index.html"

# A .nojekyll keeps GitHub Pages from mangling the _-prefixed emscripten files.
touch "$OUT_DIR/.nojekyll"

# Copy static share assets (social-card image for OpenGraph/Twitter unfurls).
if [ -f "$SCRIPT_DIR/og-image.png" ]; then
	cp "$SCRIPT_DIR/og-image.png" "$OUT_DIR/og-image.png"
fi

echo "Done. Output in $OUT_DIR/"
