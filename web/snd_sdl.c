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
// snd_sdl.c -- SDL2 audio output (DMA emulation) for the Emscripten/web port
//
// The Quake mixer only produces 8- or 16-bit PCM, but Emscripten's SDL backend
// hands us 32-bit float output. We therefore always run the engine ring buffer
// at 16-bit and convert to whatever format SDL actually opened in the callback.

#include <SDL.h>
#include "quakedef.h"

#define	RING_SAMPLES	16384	// mono samples in the engine DMA ring (power of two)

static dma_t			the_shm;
static int				snd_inited = 0;
static SDL_AudioSpec	obtained;


/*
================
paint_audio

SDL audio callback. Pull mixed 16-bit samples out of the engine ring buffer and
emit them in SDL's negotiated sample format.
================
*/
static void paint_audio (void *unused, Uint8 *stream, int len)
{
	short	*ring;
	int		ringsamples;
	int		outbytes;
	int		nitems, i;

	if (!shm || !shm->buffer)
	{
		memset (stream, 0, len);
		return;
	}

	ring = (short *) shm->buffer;
	ringsamples = shm->samples;	// total mono samples in the ring
	outbytes = SDL_AUDIO_BITSIZE (obtained.format) / 8;
	nitems = len / outbytes;	// number of sample items SDL wants

	if (obtained.format == AUDIO_S16SYS)
	{
		short	*out = (short *) stream;
		for (i = 0; i < nitems; i++)
		{
			out[i] = ring[shm->samplepos];
			if (++shm->samplepos >= ringsamples)
				shm->samplepos = 0;
		}
	}
	else if (obtained.format == AUDIO_F32SYS)
	{
		float	*out = (float *) stream;
		for (i = 0; i < nitems; i++)
		{
			out[i] = ring[shm->samplepos] / 32768.0f;
			if (++shm->samplepos >= ringsamples)
				shm->samplepos = 0;
		}
	}
	else
	{
		memset (stream, 0, len);
	}
}


qboolean SNDDMA_Init (void)
{
	SDL_AudioSpec	desired;

	snd_inited = 0;

	if (SDL_InitSubSystem (SDL_INIT_AUDIO) < 0)
	{
		Con_Printf ("Couldn't init SDL audio: %s\n", SDL_GetError ());
		return false;
	}

	memset (&desired, 0, sizeof (desired));
	desired.freq = 22050;
	desired.format = AUDIO_S16SYS;	// Emscripten will hand back F32; we convert
	desired.channels = 2;
	desired.samples = 1024;
	desired.callback = paint_audio;

	// allow SDL to change the format/frequency; the callback adapts
	if (SDL_OpenAudio (&desired, &obtained) < 0)
	{
		Con_Printf ("Couldn't open SDL audio: %s\n", SDL_GetError ());
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		return false;
	}

	if (obtained.format != AUDIO_S16SYS && obtained.format != AUDIO_F32SYS)
	{
		Con_Printf ("SDL audio format %d unsupported.\n", obtained.format);
		SDL_CloseAudio ();
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		return false;
	}

	memset (&the_shm, 0, sizeof (the_shm));
	shm = &the_shm;

	// engine ring buffer is always signed 16-bit
	shm->splitbuffer = 0;
	shm->samplebits = 16;
	shm->speed = obtained.freq;
	shm->channels = obtained.channels;
	shm->samples = RING_SAMPLES;	// mono samples (must be a multiple of channels)
	shm->samplepos = 0;
	shm->submission_chunk = 1;

	shm->buffer = (unsigned char *) calloc (shm->samples, sizeof (short));
	if (!shm->buffer)
	{
		Con_Printf ("Couldn't allocate sound buffer\n");
		SDL_CloseAudio ();
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		shm = NULL;
		return false;
	}

	SDL_PauseAudio (0);

	snd_inited = 1;
	return true;
}


int SNDDMA_GetDMAPos (void)
{
	if (!snd_inited || !shm)
		return 0;

	return shm->samplepos;
}


void SNDDMA_Shutdown (void)
{
	if (snd_inited)
	{
		SDL_PauseAudio (1);
		SDL_CloseAudio ();
		SDL_QuitSubSystem (SDL_INIT_AUDIO);
		snd_inited = 0;
	}

	if (shm && shm->buffer)
	{
		free (shm->buffer);
		shm->buffer = NULL;
	}
	shm = NULL;
}


/*
================
SNDDMA_Submit

The SDL callback pulls samples directly from the ring buffer, so there is
nothing to push here.
================
*/
void SNDDMA_Submit (void)
{
}


#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// ---------------------------------------------------------------------------
// Background music streaming.
//
// Streams audio from a hidden, off-screen SoundCloud player iframe. No UI is
// shown in the game window -- only the audio plays. Toggled from the in-game
// Options menu ("Stream Music"). Quake's original score was Trent Reznor /
// Nine Inch Nails, so dark industrial is the canonical vibe; the default feed
// is the NIN profile. The embed only resolves real SoundCloud resources (a
// track, playlist, or user) -- not /tags/ URLs.
// ---------------------------------------------------------------------------
EM_JS(void, Web_MusicStreamJS, (int on), {
	var id = 'qk-music-stream';
	var el = document.getElementById(id);
	if (on) {
		if (!el) {
			el = document.createElement('iframe');
			el.id = id;
			el.allow = 'autoplay';
			// Kept in the DOM but hidden off-screen; the audio still plays.
			el.style.cssText = 'position:absolute;width:1px;height:1px;' +
				'left:-9999px;top:-9999px;border:0;visibility:hidden;';
			el.src = 'https://w.soundcloud.com/player/?url=' +
				encodeURIComponent('https://soundcloud.com/nineinchnails') +
				'&auto_play=true&hide_related=true&show_comments=false&visual=false';
			document.body.appendChild(el);

			// Hook the SoundCloud Widget API so the in-game Music Volume slider
			// can drive playback volume. The desired level is stashed on a
			// global and (re)applied once the widget signals READY.
			var bind = function () {
				if (!window.SC || !window.SC.Widget)
					return;
				var w = SC.Widget(el);
				window.__qkWidget = w;
				w.bind(SC.Widget.Events.READY, function () {
					var v = (window.__qkMusicVol == null) ? 100 : window.__qkMusicVol;
					w.setVolume(v);
				});
			};
			if (window.SC && window.SC.Widget) {
				bind();
			} else if (!document.getElementById('qk-sc-api')) {
				var s = document.createElement('script');
				s.id = 'qk-sc-api';
				s.src = 'https://w.soundcloud.com/player/api.js';
				s.onload = bind;
				document.body.appendChild(s);
			} else {
				var tries = 0;
				var iv = setInterval(function () {
					if (window.SC && window.SC.Widget) { clearInterval(iv); bind(); }
					else if (++tries > 50) { clearInterval(iv); }
				}, 100);
			}
		}
	} else if (el && el.parentNode) {
		el.parentNode.removeChild(el);
		window.__qkWidget = null;
	}
});

EM_JS(void, Web_SetMusicVolumeJS, (int vol), {
	window.__qkMusicVol = vol;
	if (window.__qkWidget && window.__qkWidget.setVolume)
		window.__qkWidget.setVolume(vol);
});

static int web_music_on = 0;

void Web_ToggleMusic (void)
{
	web_music_on = !web_music_on;
	Web_MusicStreamJS (web_music_on);
}

int Web_MusicState (void)
{
	return web_music_on;
}

// vol01 is the engine's 0..1 music level; the widget wants 0..100.
void Web_SetMusicVolume (float vol01)
{
	int v = (int)(vol01 * 100.0f + 0.5f);
	if (v < 0)	v = 0;
	if (v > 100)	v = 100;
	Web_SetMusicVolumeJS (v);
}
#endif
