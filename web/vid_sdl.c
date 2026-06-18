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

viddef_t	vid;				// global video state

unsigned short	d_8to16table[256];
unsigned	d_8to24table[256];

// 32-bit (ARGB8888) lookup built from the current 8-bit palette
static unsigned	st2d_8to32table[256];

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

	// translate the whole 8-bit frame into the ARGB present buffer
	src = vid.buffer;
	dst = argbbuffer;
	count = vid.width * vid.height;
	for (i = 0; i < count; i++)
		dst[i] = st2d_8to32table[src[i]];

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
