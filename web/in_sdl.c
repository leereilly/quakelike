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
// in_sdl.c -- SDL2 keyboard + mouse input for the Emscripten/web port

#include <SDL.h>
#include "quakedef.h"

static cvar_t	m_filter = {"m_filter", "0"};

static qboolean	mouse_avail = false;
static qboolean	mouse_active = false;
static float	mouse_x, mouse_y;
static float	old_mouse_x, old_mouse_y;

extern qboolean	noclip_anglehack;


/*
===========
IN_MapKey

Translate an SDL keysym into a Quake key code.
===========
*/
static int IN_MapKey (SDL_Keysym *keysym)
{
	int	sym = keysym->sym;

	switch (sym)
	{
	case SDLK_TAB:			return K_TAB;
	case SDLK_RETURN:		return K_ENTER;
	case SDLK_RETURN2:		return K_ENTER;
	case SDLK_KP_ENTER:		return K_ENTER;
	case SDLK_ESCAPE:		return K_ESCAPE;
	case SDLK_SPACE:		return K_SPACE;
	case SDLK_BACKSPACE:	return K_BACKSPACE;

	case SDLK_UP:			return K_UPARROW;
	case SDLK_DOWN:			return K_DOWNARROW;
	case SDLK_LEFT:			return K_LEFTARROW;
	case SDLK_RIGHT:		return K_RIGHTARROW;

	case SDLK_LALT:
	case SDLK_RALT:			return K_ALT;
	case SDLK_LCTRL:
	case SDLK_RCTRL:		return K_CTRL;
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:		return K_SHIFT;

	case SDLK_F1:			return K_F1;
	case SDLK_F2:			return K_F2;
	case SDLK_F3:			return K_F3;
	case SDLK_F4:			return K_F4;
	case SDLK_F5:			return K_F5;
	case SDLK_F6:			return K_F6;
	case SDLK_F7:			return K_F7;
	case SDLK_F8:			return K_F8;
	case SDLK_F9:			return K_F9;
	case SDLK_F10:			return K_F10;
	case SDLK_F11:			return K_F11;
	case SDLK_F12:			return K_F12;

	case SDLK_INSERT:		return K_INS;
	case SDLK_DELETE:		return K_DEL;
	case SDLK_PAGEDOWN:		return K_PGDN;
	case SDLK_PAGEUP:		return K_PGUP;
	case SDLK_HOME:			return K_HOME;
	case SDLK_END:			return K_END;
	case SDLK_PAUSE:		return K_PAUSE;

	// keypad as digits / arrows
	case SDLK_KP_0:			return K_INS;
	case SDLK_KP_1:			return K_END;
	case SDLK_KP_2:			return K_DOWNARROW;
	case SDLK_KP_3:			return K_PGDN;
	case SDLK_KP_4:			return K_LEFTARROW;
	case SDLK_KP_5:			return '5';
	case SDLK_KP_6:			return K_RIGHTARROW;
	case SDLK_KP_7:			return K_HOME;
	case SDLK_KP_8:			return K_UPARROW;
	case SDLK_KP_9:			return K_PGUP;
	case SDLK_KP_PERIOD:	return K_DEL;
	case SDLK_KP_DIVIDE:	return '/';
	case SDLK_KP_MULTIPLY:	return '*';
	case SDLK_KP_MINUS:		return '-';
	case SDLK_KP_PLUS:		return '+';

	default:
		// printable ASCII; Quake wants lowercased ascii for normal keys
		if (sym >= SDLK_SPACE && sym < 128)
			return sym;
		return 0;
	}
}


static void IN_ActivateMouse (void)
{
	if (mouse_avail && !mouse_active)
	{
		SDL_SetRelativeMouseMode (SDL_TRUE);
		SDL_GetRelativeMouseState (NULL, NULL);	// clear accumulated delta
		mouse_active = true;
	}
}

static void IN_DeactivateMouse (void)
{
	if (mouse_active)
	{
		SDL_SetRelativeMouseMode (SDL_FALSE);
		mouse_active = false;
	}
}


/*
===========
Sys_SendKeyEvents

Pump the SDL event queue. Called by the engine once per frame.
===========
*/
void Sys_SendKeyEvents (void)
{
	SDL_Event	event;
	int			key;

	while (SDL_PollEvent (&event))
	{
		switch (event.type)
		{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			key = IN_MapKey (&event.key.keysym);
			if (key)
				Key_Event (key, event.type == SDL_KEYDOWN);
			break;

		case SDL_MOUSEMOTION:
			if (mouse_active)
			{
				mouse_x += event.motion.xrel;
				mouse_y += event.motion.yrel;
			}
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			{
				qboolean down = (event.type == SDL_MOUSEBUTTONDOWN);

				if (down && !mouse_active)
					IN_ActivateMouse ();

				switch (event.button.button)
				{
				case SDL_BUTTON_LEFT:	Key_Event (K_MOUSE1, down); break;
				case SDL_BUTTON_RIGHT:	Key_Event (K_MOUSE2, down); break;
				case SDL_BUTTON_MIDDLE:	Key_Event (K_MOUSE3, down); break;
				default: break;
				}
			}
			break;

		case SDL_MOUSEWHEEL:
			if (event.wheel.y > 0)
			{
				Key_Event (K_MWHEELUP, true);
				Key_Event (K_MWHEELUP, false);
			}
			else if (event.wheel.y < 0)
			{
				Key_Event (K_MWHEELDOWN, true);
				Key_Event (K_MWHEELDOWN, false);
			}
			break;

		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				IN_DeactivateMouse ();
			break;

		case SDL_QUIT:
			Sys_Quit ();
			break;

		default:
			break;
		}
	}
}


void IN_Init (void)
{
	Cvar_RegisterVariable (&m_filter);

	if (COM_CheckParm ("-nomouse"))
		mouse_avail = false;
	else
		mouse_avail = true;
}

void IN_Shutdown (void)
{
	IN_DeactivateMouse ();
	mouse_avail = false;
}

void IN_Commands (void)
{
	// mouse buttons are handled in the event pump
}

void IN_Move (usercmd_t *cmd)
{
	if (!mouse_active)
		return;

	if (m_filter.value)
	{
		mouse_x = (mouse_x + old_mouse_x) * 0.5;
		mouse_y = (mouse_y + old_mouse_y) * 0.5;
	}

	old_mouse_x = mouse_x;
	old_mouse_y = mouse_y;

	mouse_x *= sensitivity.value;
	mouse_y *= sensitivity.value;

	if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;

	if (in_mlook.state & 1)
		V_StopPitchDrift ();

	if ((in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}

	mouse_x = mouse_y = 0;
}
