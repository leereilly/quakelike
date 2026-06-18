/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_sdl.c -- SDL2 software-renderer video driver for the Emscripten/web port

#include <SDL.h>
#include "quakedef.h"
#include "d_local.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

viddef_t	vid;				// global video state

unsigned short	d_8to16table[256];
unsigned	d_8to24table[256];

// 32-bit (ARGB8888) lookup built from the current 8-bit palette (true colors)
static unsigned	st2d_8to32table[256];
// the table VID_Update actually samples: true colors with the active filter applied
static unsigned	st2d_active[256];

// a copy of the current 8-bit palette, so filters can be rebuilt on the fly
static unsigned char	vid_curpal[768];

// ---------------------------------------------------------------------------
// Post-palette "video filters" -- because the renderer is fully palettized,
// remapping the 256-entry color table recolors the entire game for almost no
// cost. vid_filter selects the look; the table is rebuilt when it changes.
// ---------------------------------------------------------------------------
enum {
	VID_FILTER_NONE = 0,
	VID_FILTER_THERMAL,		// RED HOT / predator thermal vision
	VID_FILTER_SYNTHWAVE,	// neon magenta -> cyan
	VID_FILTER_MATRIX,		// green phosphor monochrome
	VID_FILTER_COUNT
};

cvar_t	vid_filter = {"vid_filter", "0", true};
static int	vid_filter_current = -1;

void (*vid_menudrawfn)(void);
void (*vid_menukeyfn)(int key);

static SDL_Window	*sdl_window = NULL;
static SDL_Renderer	*sdl_renderer = NULL;
static SDL_Texture	*sdl_texture = NULL;

static unsigned		*argbbuffer = NULL;	// vid.width * vid.height ARGB pixels
static int			vid_surfcachesize;
static void			*vid_surfcache;
static int			VID_highhunkmark;

#define	BASEWIDTH	320
#define	BASEHEIGHT	200

// default on-screen scale (CSS / window enlargement of the 320x200 image)
#define	DEFAULT_SCALE	2

static int	vid_initialized = 0;


/*
================
VID_AllocBuffers

Allocate the 8-bit drawing buffer, the z-buffer and the surface cache on the
high hunk, mirroring the layout used by the original software drivers.
================
*/
static void VID_AllocBuffers (void)
{
	int		buffersize;

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (VID_highhunkmark);
		d_pzbuffer = NULL;
	}

	VID_highhunkmark = Hunk_HighMark ();

	// 8-bit color buffer + z-buffer + surface cache, all in one block
	buffersize = vid.width * vid.height * sizeof (*d_pzbuffer);

	vid_surfcachesize = D_SurfaceCacheForRes (vid.width, vid.height);

	buffersize += vid_surfcachesize;
	buffersize += vid.width * vid.height * sizeof (pixel_t);

	d_pzbuffer = Hunk_HighAllocName (buffersize, "video");
	if (d_pzbuffer == NULL)
		Sys_Error ("Not enough memory for video mode\n");

	vid_surfcache = (byte *) d_pzbuffer
		+ vid.width * vid.height * sizeof (*d_pzbuffer);

	D_InitCaches (vid_surfcache, vid_surfcachesize);

	vid.buffer = (pixel_t *)((byte *)vid_surfcache + vid_surfcachesize);
	vid.conbuffer = vid.buffer;
}


static unsigned VID_PackARGB (int r, int g, int b)
{
	if (r < 0) r = 0; else if (r > 255) r = 255;
	if (g < 0) g = 0; else if (g > 255) g = 255;
	if (b < 0) b = 0; else if (b > 255) b = 255;
	return (0xFFu << 24) | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

/*
================
VID_BuildFilter

Rebuild st2d_active[] from the stored palette, applying the currently selected
video filter. Called whenever the palette or vid_filter changes.
================
*/
void	VID_BuildFilter (void)
{
	int		i, filter;
	unsigned char	*pal = vid_curpal;

	filter = (int)vid_filter.value;
	if (filter < 0 || filter >= VID_FILTER_COUNT)
		filter = VID_FILTER_NONE;
	vid_filter_current = filter;

	for (i = 0; i < 256; i++, pal += 3)
	{
		int r = pal[0];
		int g = pal[1];
		int b = pal[2];
		int lum = (r * 77 + g * 150 + b * 29) >> 8;	// 0..255 luminance

		switch (filter)
		{
		default:
		case VID_FILTER_NONE:
			st2d_active[i] = st2d_8to32table[i];
			break;

		case VID_FILTER_THERMAL:
			// black -> red -> orange -> yellow -> white heat ramp
			if (lum < 85)
				st2d_active[i] = VID_PackARGB (lum * 3, 0, 0);
			else if (lum < 170)
				st2d_active[i] = VID_PackARGB (255, (lum - 85) * 3, 0);
			else
				st2d_active[i] = VID_PackARGB (255, 255, (lum - 170) * 3);
			break;

		case VID_FILTER_SYNTHWAVE:
			// deep purple -> hot magenta -> cyan
			if (lum < 128)
			{
				int t = lum * 2;	// 0..255
				st2d_active[i] = VID_PackARGB (
					20 + t * 235 / 255,
					t * 20 / 255,
					40 + t * 107 / 255);
			}
			else
			{
				int t = (lum - 128) * 2;	// 0..255
				st2d_active[i] = VID_PackARGB (
					255 - t * 255 / 255,
					20 + t * 210 / 255,
					147 + t * 108 / 255);
			}
			break;

		case VID_FILTER_MATRIX:
			// green phosphor monochrome
			st2d_active[i] = VID_PackARGB (lum / 6, lum + (lum >> 3), lum / 5);
			break;
		}
	}
}

/*
================
VID_CycleFilter_f

Console command "vidfilter": advance to the next video filter.
================
*/
void	VID_CycleFilter_f (void)
{
	int	next = (vid_filter_current + 1) % VID_FILTER_COUNT;
	Cvar_SetValue ("vid_filter", (float)next);
	VID_BuildFilter ();
}

#ifdef __EMSCRIPTEN__
// Called from the HTML shell's filter buttons.
EMSCRIPTEN_KEEPALIVE void Web_SetFilter (int n)
{
	if (n < 0 || n >= VID_FILTER_COUNT)
		n = VID_FILTER_NONE;
	Cvar_SetValue ("vid_filter", (float)n);
	VID_BuildFilter ();
}
#endif


void	VID_SetPalette (unsigned char *palette)
{
	int		i;
	unsigned char	*pal = palette;

	for (i = 0; i < 256; i++)
	{
		unsigned r = pal[0];
		unsigned g = pal[1];
		unsigned b = pal[2];
		pal += 3;

		st2d_8to32table[i] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
		d_8to24table[i] = (r << 16) | (g << 8) | b;
	}

	// keep a copy so the active filter table can be rebuilt at any time
	memcpy (vid_curpal, palette, sizeof (vid_curpal));
	VID_BuildFilter ();
}

void	VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}


void	VID_Init (unsigned char *palette)
{
	int		pnum;
	int		scale = DEFAULT_SCALE;

	vid.width = BASEWIDTH;
	vid.height = BASEHEIGHT;

	if ((pnum = COM_CheckParm ("-width")) && pnum < com_argc - 1)
		vid.width = Q_atoi (com_argv[pnum + 1]);
	if ((pnum = COM_CheckParm ("-height")) && pnum < com_argc - 1)
		vid.height = Q_atoi (com_argv[pnum + 1]);
	if ((pnum = COM_CheckParm ("-winsize")) && pnum < com_argc - 2)
	{
		vid.width = Q_atoi (com_argv[pnum + 1]);
		vid.height = Q_atoi (com_argv[pnum + 2]);
	}
	if ((pnum = COM_CheckParm ("-scale")) && pnum < com_argc - 1)
		scale = Q_atoi (com_argv[pnum + 1]);

	if (vid.width < 320)
		vid.width = 320;
	if (vid.height < 200)
		vid.height = 200;
	// the software water-warp scratch buffers cap the render resolution
	if (vid.width > WARP_WIDTH)
		vid.width = WARP_WIDTH;
	if (vid.height > WARP_HEIGHT)
		vid.height = WARP_HEIGHT;
	if (scale < 1)
		scale = 1;

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.rowbytes = vid.width;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	vid.conwidth = vid.width;
	vid.conheight = vid.height;
	vid.conrowbytes = vid.rowbytes;
	vid.direct = 0;

	if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
		Sys_Error ("VID: Couldn't init SDL video: %s\n", SDL_GetError ());

	sdl_window = SDL_CreateWindow ("Quake",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		vid.width * scale, vid.height * scale, SDL_WINDOW_SHOWN);
	if (!sdl_window)
		Sys_Error ("VID: Couldn't create window: %s\n", SDL_GetError ());

	sdl_renderer = SDL_CreateRenderer (sdl_window, -1, 0);
	if (!sdl_renderer)
		Sys_Error ("VID: Couldn't create renderer: %s\n", SDL_GetError ());

	SDL_RenderSetLogicalSize (sdl_renderer, vid.width, vid.height);

	sdl_texture = SDL_CreateTexture (sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING, vid.width, vid.height);
	if (!sdl_texture)
		Sys_Error ("VID: Couldn't create texture: %s\n", SDL_GetError ());

	argbbuffer = (unsigned *) malloc (vid.width * vid.height * sizeof (unsigned));
	if (!argbbuffer)
		Sys_Error ("VID: Out of memory for the present buffer\n");

	VID_SetPalette (palette);
	VID_AllocBuffers ();

	Cvar_RegisterVariable (&vid_filter);
	Cmd_AddCommand ("vidfilter", VID_CycleFilter_f);
	VID_BuildFilter ();

	vid_initialized = 1;
}


void	VID_Shutdown (void)
{
	if (!vid_initialized)
		return;
	vid_initialized = 0;

	if (argbbuffer)
	{
		free (argbbuffer);
		argbbuffer = NULL;
	}
	if (sdl_texture)
		SDL_DestroyTexture (sdl_texture);
	if (sdl_renderer)
		SDL_DestroyRenderer (sdl_renderer);
	if (sdl_window)
		SDL_DestroyWindow (sdl_window);
	sdl_texture = NULL;
	sdl_renderer = NULL;
	sdl_window = NULL;

	SDL_QuitSubSystem (SDL_INIT_VIDEO);
}


void	VID_Update (vrect_t *rects)
{
	int			i, count;
	pixel_t		*src;
	unsigned	*dst;

	if (!vid_initialized)
		return;

	// allow the filter to be changed via the vid_filter cvar (console)
	if ((int)vid_filter.value != vid_filter_current)
		VID_BuildFilter ();

	// translate the whole 8-bit frame into the ARGB present buffer
	src = vid.buffer;
	dst = argbbuffer;
	count = vid.width * vid.height;
	for (i = 0; i < count; i++)
		dst[i] = st2d_active[src[i]];

	SDL_UpdateTexture (sdl_texture, NULL, argbbuffer,
		vid.width * sizeof (unsigned));
	SDL_RenderClear (sdl_renderer);
	SDL_RenderCopy (sdl_renderer, sdl_texture, NULL, NULL);
	SDL_RenderPresent (sdl_renderer);
}


/*
================
D_BeginDirectRect / D_EndDirectRect

These draw directly to the framebuffer (used for the loading/pause icons and
the disc-access blip). vid.direct is NULL on this driver, so the engine routes
those through the normal buffer; nothing to do here.
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}

void D_EndDirectRect (int x, int y, int width, int height)
{
}


int VID_SetMode (int modenum, unsigned char *palette)
{
	VID_SetPalette (palette);
	return 1;
}

void VID_HandlePause (qboolean pause)
{
}
