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
// sys_sdl.c -- system layer + main loop for the Emscripten/web port

#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "quakedef.h"

qboolean	isDedicated;

int		nostdout = 0;

char		*basedir = ".";
char		*cachedir = "/tmp";

cvar_t		sys_linerefresh = {"sys_linerefresh", "0"};

// =======================================================================
// General routines
// =======================================================================

void Sys_DebugNumber (int y, int val)
{
}

void Sys_Printf (char *fmt, ...)
{
	va_list		argptr;
	char		text[1024];
	unsigned char	*p;

	va_start (argptr, fmt);
	vsnprintf (text, sizeof (text), fmt, argptr);
	va_end (argptr);

	if (nostdout)
		return;

	for (p = (unsigned char *)text; *p; p++)
	{
		*p &= 0x7f;
		if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
			printf ("[%02x]", *p);
		else
			putc (*p, stdout);
	}
	fflush (stdout);
}

void Sys_Quit (void)
{
	Host_Shutdown ();
#ifdef __EMSCRIPTEN__
	emscripten_cancel_main_loop ();
#endif
	exit (0);
}

void Sys_Init (void)
{
}

void Sys_Error (char *error, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr, error);
	vsnprintf (string, sizeof (string), error, argptr);
	va_end (argptr);
	fprintf (stderr, "Error: %s\n", string);

	Host_Shutdown ();
#ifdef __EMSCRIPTEN__
	emscripten_cancel_main_loop ();
	emscripten_force_exit (1);
#endif
	exit (1);
}

void Sys_Warn (char *warning, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr, warning);
	vsnprintf (string, sizeof (string), warning, argptr);
	va_end (argptr);
	fprintf (stderr, "Warning: %s", string);
}

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int	Sys_FileTime (char *path)
{
	struct stat	buf;

	if (stat (path, &buf) == -1)
		return -1;

	return buf.st_mtime;
}

void Sys_mkdir (char *path)
{
	mkdir (path, 0777);
}

int Sys_FileOpenRead (char *path, int *handle)
{
	int		h;
	struct stat	fileinfo;

	h = open (path, O_RDONLY, 0666);
	*handle = h;
	if (h == -1)
		return -1;

	if (fstat (h, &fileinfo) == -1)
		Sys_Error ("Error fstating %s", path);

	return fileinfo.st_size;
}

int Sys_FileOpenWrite (char *path)
{
	int		handle;

	umask (0);

	handle = open (path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (handle == -1)
		Sys_Error ("Error opening %s", path);

	return handle;
}

int Sys_FileWrite (int handle, void *src, int count)
{
	return write (handle, src, count);
}

void Sys_FileClose (int handle)
{
	close (handle);
}

void Sys_FileSeek (int handle, int position)
{
	lseek (handle, position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	return read (handle, dest, count);
}

void Sys_DebugLog (char *file, char *fmt, ...)
{
	va_list	argptr;
	static char data[1024];
	int	fd;

	va_start (argptr, fmt);
	vsnprintf (data, sizeof (data), fmt, argptr);
	va_end (argptr);
	fd = open (file, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd != -1)
	{
		write (fd, data, strlen (data));
		close (fd);
	}
}

void Sys_EditFile (char *filename)
{
}

double Sys_FloatTime (void)
{
	struct timeval	tp;
	struct timezone	tzp;
	static int	secbase;

	gettimeofday (&tp, &tzp);

	if (!secbase)
	{
		secbase = tp.tv_sec;
		return tp.tv_usec / 1000000.0;
	}

	return (tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
}

void Sys_LineRefresh (void)
{
}

char *Sys_ConsoleInput (void)
{
	return NULL;
}

void Sys_HighFPPrecision (void)
{
}

void Sys_LowFPPrecision (void)
{
}

/*
================
Sys_MakeCodeWriteable

No self-modifying code on wasm; nothing to do.
================
*/
void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
}


// =======================================================================
// Main loop
// =======================================================================

static double	host_oldtime;

static void Sys_Frame (void)
{
	double	newtime, frametime;

	newtime = Sys_FloatTime ();
	frametime = newtime - host_oldtime;
	host_oldtime = newtime;

	if (frametime > 0.25)
		frametime = 0.25;	// clamp after a stall (tab switch, etc.)

	Host_Frame (frametime);
}

int main (int c, char **v)
{
	quakeparms_t	parms;
	int		j;

	memset (&parms, 0, sizeof (parms));

	COM_InitArgv (c, v);
	parms.argc = com_argc;
	parms.argv = com_argv;

	parms.memsize = 16 * 1024 * 1024;

	j = COM_CheckParm ("-mem");
	if (j)
		parms.memsize = (int)(Q_atof (com_argv[j + 1]) * 1024 * 1024);
	parms.membase = malloc (parms.memsize);
	if (!parms.membase)
		Sys_Error ("Not enough memory for the Quake heap\n");

	parms.basedir = basedir;

	Host_Init (&parms);

	Sys_Init ();

	printf ("Web Quake -- software renderer\n");

	host_oldtime = Sys_FloatTime () - 0.1;

#ifdef __EMSCRIPTEN__
	// 0 fps => drive from requestAnimationFrame; do not simulate infinite loop
	emscripten_set_main_loop (Sys_Frame, 0, 0);
#else
	while (1)
		Sys_Frame ();
#endif

	return 0;
}
