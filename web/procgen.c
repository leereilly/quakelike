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
// procgen.c -- procedural Quake BSP29 generator for the web port.
//
// Quake levels are normally compiled from .map files by qbsp/vis/light. Since
// none of those tools exist in the browser, this builds a *valid* BSP29 image
// directly in memory: planes, vertices, edges, faces, an embedded wall texture,
// the rendering BSP (nodes/leafs), and the collision clip hulls. The result is
// written into the in-memory filesystem and handed to the normal map loader.
//
// v1 generated a single room; this builds a connected grid of rooms (a small
// dungeon) as a k-d tree BSP over the cell grid, with matching clip hulls.

#include "quakedef.h"

#include <emscripten.h>
#include <stdio.h>
#include <sys/stat.h>

//=========================================================================
// On-disk BSP29 structures (mirrors bspfile.h; redefined locally so this
// file is self-contained and the byte layout is explicit).
//=========================================================================

#define	PG_BSPVERSION	29

#define	PG_LUMP_ENTITIES	0
#define	PG_LUMP_PLANES		1
#define	PG_LUMP_TEXTURES	2
#define	PG_LUMP_VERTEXES	3
#define	PG_LUMP_VISIBILITY	4
#define	PG_LUMP_NODES		5
#define	PG_LUMP_TEXINFO		6
#define	PG_LUMP_FACES		7
#define	PG_LUMP_LIGHTING	8
#define	PG_LUMP_CLIPNODES	9
#define	PG_LUMP_LEAFS		10
#define	PG_LUMP_MARKSURFACES	11
#define	PG_LUMP_EDGES		12
#define	PG_LUMP_SURFEDGES	13
#define	PG_LUMP_MODELS		14
#define	PG_HEADER_LUMPS		15

#define	PG_CONTENTS_EMPTY	-1
#define	PG_CONTENTS_SOLID	-2

#define	PG_PLANE_X	0
#define	PG_PLANE_Y	1
#define	PG_PLANE_Z	2

typedef struct { int fileofs, filelen; } pg_lump_t;

typedef struct { int version; pg_lump_t lumps[PG_HEADER_LUMPS]; } pg_header_t;

typedef struct { float point[3]; } pg_vertex_t;

typedef struct { float normal[3]; float dist; int type; } pg_plane_t;

typedef struct {
	int		planenum;
	short		children[2];
	short		mins[3];
	short		maxs[3];
	unsigned short	firstface;
	unsigned short	numfaces;
} pg_node_t;

typedef struct { int planenum; short children[2]; } pg_clipnode_t;

typedef struct { float vecs[2][4]; int miptex; int flags; } pg_texinfo_t;

typedef struct { unsigned short v[2]; } pg_edge_t;

typedef struct {
	short	planenum;
	short	side;
	int	firstedge;
	short	numedges;
	short	texinfo;
	unsigned char	styles[4];
	int	lightofs;
} pg_face_t;

typedef struct {
	int		contents;
	int		visofs;
	short		mins[3];
	short		maxs[3];
	unsigned short	firstmarksurface;
	unsigned short	nummarksurfaces;
	unsigned char	ambient_level[4];
} pg_leaf_t;

typedef struct {
	float	mins[3], maxs[3];
	float	origin[3];
	int	headnode[4];
	int	visleafs;
	int	firstface, numfaces;
} pg_model_t;

//=========================================================================
// Builder state
//=========================================================================

#define	PG_MAX_PLANES		8192
#define	PG_MAX_VERTS		16384
#define	PG_MAX_EDGES		32768
#define	PG_MAX_SURFEDGES	32768
#define	PG_MAX_FACES		8192
#define	PG_MAX_TEXINFO		64
#define	PG_MAX_NODES		8192
#define	PG_MAX_LEAFS		4096
#define	PG_MAX_CLIPNODES	16384
#define	PG_MAX_MARKSURF		32768
#define	PG_ENTSTRING		32768
#define	PG_MAPBUF_CAP		(2 * 1024 * 1024)	// serialize scratch buffer

typedef struct {
	pg_plane_t	planes[PG_MAX_PLANES];		int numplanes;
	pg_vertex_t	verts[PG_MAX_VERTS];		int numverts;
	pg_edge_t	edges[PG_MAX_EDGES];		int numedges;
	int		surfedges[PG_MAX_SURFEDGES];	int numsurfedges;
	pg_face_t	faces[PG_MAX_FACES];		int numfaces;
	pg_texinfo_t	texinfos[PG_MAX_TEXINFO];	int numtexinfo;
	pg_node_t	nodes[PG_MAX_NODES];		int numnodes;
	pg_leaf_t	leafs[PG_MAX_LEAFS];		int numleafs;
	pg_clipnode_t	clipnodes[PG_MAX_CLIPNODES];	int numclipnodes;
	unsigned short	marksurf[PG_MAX_MARKSURF];	int nummarksurf;
	char		entities[PG_ENTSTRING];		int entlen;
} pg_build_t;

static pg_build_t	pg;

//=========================================================================
// Dungeon layout
//
// Rooms sit on an interleaved grid: even cell lines are rooms, odd lines are
// the thin walls between them. A wall cell is opened only where the layout
// connects two rooms (a passage); the rest stay solid, and the odd/odd corner
// cells are always solid, so the dungeon is divided by real walls and studded
// with square pillars rather than being one open hall.
//=========================================================================

#define	PG_ROOMS_MIN	6		// min rooms per axis (keeps maps large)
#define	PG_ROOMS_MAX	9		// max rooms per axis
#define	PG_ROOMS	PG_ROOMS_MAX	// grid is sized for the largest case
#define	PG_NCELL	(2 * PG_ROOMS - 1)	// interleaved cells per axis
#define	PG_ROOMMIN	192		// room footprint range (<=256 -> legal extents)
#define	PG_ROOMMAX	256
#define	PG_WALLSZ	96		// thickness of walls / depth of passages (corridor throats)
#define	PG_HMIN		176
#define	PG_HMAX		224

static int		pg_nx, pg_ny;			// interleaved grid size
static int		pg_gx[PG_NCELL + 1];		// world X of each cell line
static int		pg_gy[PG_NCELL + 1];		// world Y of each cell line
static unsigned char	pg_cellopen[PG_NCELL][PG_NCELL];	// 1 == walkable
static unsigned char	pg_walltex[PG_NCELL][PG_NCELL];	// wall texture per cell
static unsigned char	pg_floortex[PG_NCELL][PG_NCELL];// floor texture per cell
static int		pg_height;			// shared ceiling height
static int		pg_start_cx, pg_start_cy;	// spawn cell

// Slipgate: a framed warp surface set into one wall of the exit room. Reaching
// it (after the level is cleared) warps the player to a fresh random dungeon.
static int		pg_portal_cx, pg_portal_cy;	// exit room cell
static int		pg_portal_side;			// 0=-X 1=+X 2=-Y 3=+Y
static qboolean		pg_have_portal;			// a portal was placed
static int		pg_portal_x, pg_portal_y, pg_portal_z;	// world centre
static qboolean		pg_portal_open;			// level cleared, gate live
static qboolean		pg_portal_announced;		// centerprint shown
static qboolean		pg_portal_used;			// transport in flight

static int		pg_solidleaf;			// shared solid leaf index
static int		pg_render_root;			// model headnode[0]
static int		pg_hull1_root, pg_hull2_root;	// model headnode[1..2]

// Doors: a subset of passages become brush submodels (a func_door "*N"). Each
// door slides straight up out of the way; some are auto-opening, some need a
// silver/gold key. The plain framed openings (no brush) are the rest.
#define	PG_MAXDOORS	48
#define	PG_DOOR_AUTO	0
#define	PG_DOOR_SILVER	1		// needs item_key1 (silver key)
#define	PG_DOOR_GOLD	2		// needs item_key2 (gold key)
typedef struct {
	int	cx, cy;				// passage cell hosting the door
	int	type;				// PG_DOOR_*
	int	submodel;			// bsp model index (1-based)
} pg_door_t;
static pg_door_t	pg_doors[PG_MAXDOORS];
static int		pg_ndoors;
static pg_model_t	pg_doormodels[PG_MAXDOORS];
static int		pg_door_emptyleaf;		// shared empty leaf for door exteriors
static unsigned char	pg_locked[PG_NCELL][PG_NCELL];	// 1 == passage gated by a key
static int		pg_worldfaces, pg_worldleafs;	// counts before door geometry
static qboolean		pg_need_silver, pg_need_gold;	// a key of that colour is required
static int		pg_silverkey_x, pg_silverkey_y;	// world centre of the placed keys
static int		pg_goldkey_x, pg_goldkey_y;

// deterministic RNG (LCG)
static unsigned int	pg_seed;
static unsigned int	pg_last_seed;	// original seed of the current dungeon (for sharing)
static int pg_rand (void)
{
	pg_seed = pg_seed * 1103515245u + 12345u;
	return (pg_seed >> 16) & 0x7fff;
}
static int pg_range (int lo, int hi)	// inclusive
{
	if (hi <= lo)
		return lo;
	return lo + pg_rand () % (hi - lo + 1);
}

//=========================================================================
// Low-level adders
//=========================================================================

static int PG_AddPlane (float nx, float ny, float nz, float dist, int type)
{
	pg_plane_t *p = &pg.planes[pg.numplanes];
	p->normal[0] = nx; p->normal[1] = ny; p->normal[2] = nz;
	p->dist = dist;
	p->type = type;
	return pg.numplanes++;
}

static int PG_AddVertex (float x, float y, float z)
{
	pg_vertex_t *v = &pg.verts[pg.numverts];
	v->point[0] = x; v->point[1] = y; v->point[2] = z;
	return pg.numverts++;
}

// Add a face as a polygon of vertex indices (already wound CCW around the
// visible normal). Each edge is emitted fresh and referenced positively.
static int PG_AddFace (int planenum, int side, int texinfo, int *vlist, int n)
{
	pg_face_t	*f = &pg.faces[pg.numfaces];
	int		i, firstse;

	firstse = pg.numsurfedges;
	for (i = 0; i < n; i++)
	{
		int a = vlist[n - 1 - i];
		int b = vlist[(n - 1 - i + n - 1) % n];
		int e = pg.numedges;
		pg.edges[e].v[0] = (unsigned short)a;
		pg.edges[e].v[1] = (unsigned short)b;
		pg.numedges++;
		pg.surfedges[pg.numsurfedges++] = e;	// positive: a -> b
	}

	f->planenum = (short)planenum;
	f->side = (short)side;
	f->firstedge = firstse;
	f->numedges = (short)n;
	f->texinfo = (short)texinfo;
	f->styles[0] = 0;
	f->styles[1] = f->styles[2] = f->styles[3] = 255;
	f->lightofs = -1;			// no lightmap -> fullbright
	return pg.numfaces++;
}

// Add an axis-aligned quad: four corners given CCW around the visible
// (inward) normal. Each corner becomes a fresh vertex.
static int PG_AddQuad (int planenum, int side, int texinfo,
	float ax, float ay, float az, float bx, float by, float bz,
	float cx, float cy, float cz, float dx, float dy, float dz)
{
	int vl[4];
	vl[0] = PG_AddVertex (ax, ay, az);
	vl[1] = PG_AddVertex (bx, by, bz);
	vl[2] = PG_AddVertex (cx, cy, cz);
	vl[3] = PG_AddVertex (dx, dy, dz);
	return PG_AddFace (planenum, side, texinfo, vl, 4);
}

static int PG_AddTexinfo (float sx, float sy, float sz,
			  float tx, float ty, float tz, int miptex)
{
	pg_texinfo_t *t = &pg.texinfos[pg.numtexinfo];
	t->vecs[0][0] = sx; t->vecs[0][1] = sy; t->vecs[0][2] = sz; t->vecs[0][3] = 0;
	t->vecs[1][0] = tx; t->vecs[1][1] = ty; t->vecs[1][2] = tz; t->vecs[1][3] = 0;
	t->miptex = miptex;
	t->flags = 0;
	return pg.numtexinfo++;
}

static void PG_EntCat (const char *s)
{
	int l = strlen (s);
	if (pg.entlen + l < PG_ENTSTRING)
	{
		memcpy (pg.entities + pg.entlen, s, l);
		pg.entlen += l;
		pg.entities[pg.entlen] = 0;
	}
}

//=========================================================================
// Real textures, borrowed from a shipped map
//
// The browser has no texture WAD compiler, but the shareware maps already
// carry plenty of miptextures embedded in their TEXTURES lump. We load one
// map, copy out a handful of named textures verbatim, and assemble a compact
// dmiptexlump for our own BSP. Each texture keeps its internal mip offsets
// (which are relative to the miptex, so copying is byte-exact).
//=========================================================================

#define	PG_TEXSOURCE	"maps/e1m1.bsp"
#define	PG_MAX_TEXBYTES	(256 * 1024)

// roles let the generator pick an appropriate texture per surface
#define	PG_ROLE_WALL	0
#define	PG_ROLE_FLOOR	1
#define	PG_ROLE_CEIL	2
#define	PG_ROLE_PORTAL	3	// the slipgate surface (a '*' warp texture)
#define	PG_ROLE_FRAME	4	// the slipgate's surrounding frame

typedef struct { const char *name; int role; } pg_texpick_t;

static const pg_texpick_t pg_texpick[] = {
	{ "tech08_1",  PG_ROLE_WALL },
	{ "comp1_1",   PG_ROLE_WALL },
	{ "comp1_4",   PG_ROLE_WALL },
	{ "tech01_7",  PG_ROLE_WALL },
	{ "ecop1_1",   PG_ROLE_WALL },
	{ "twall1_1",  PG_ROLE_WALL },
	{ "twall2_2",  PG_ROLE_WALL },
	{ "tech07_2",  PG_ROLE_WALL },
	{ "sfloor4_2", PG_ROLE_FLOOR },
	{ "ground1_6", PG_ROLE_FLOOR },
	{ "tlight10",  PG_ROLE_CEIL },
	{ "*teleport", PG_ROLE_PORTAL },	// turbulent slipgate surface
	{ "slip1",     PG_ROLE_FRAME },		// gate frame / surround
};
#define	PG_NUMTEX	((int)(sizeof(pg_texpick)/sizeof(pg_texpick[0])))

static unsigned char	pg_texdata[PG_MAX_TEXBYTES];	// finished texture lump
static int		pg_texlen;
static int		pg_tix[PG_NUMTEX], pg_tiy[PG_NUMTEX], pg_tiz[PG_NUMTEX];
static int		pg_wall_opt[PG_NUMTEX], pg_num_wall;
static int		pg_floor_opt[PG_NUMTEX], pg_num_floor;
static int		pg_ceil_tex;
static int		pg_portal_tex, pg_frame_tex;

static int PG_LE32 (const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// Locate a named miptex inside a map's TEXTURES lump. Returns the offset of the
// miptex within the lump and its byte length, or 0 on failure.
static int PG_FindMiptex (const unsigned char *lump, int lumplen,
			  const char *name, int *out_len)
{
	int	nummip, i, dofs, w, h, blob;

	if (lumplen < 4)
		return 0;
	nummip = PG_LE32 (lump);
	for (i = 0; i < nummip; i++)
	{
		dofs = PG_LE32 (lump + 4 + i * 4);
		if (dofs < 0 || dofs + 40 > lumplen)
			continue;
		if (Q_strcasecmp ((char *)lump + dofs, name))
			continue;
		w = PG_LE32 (lump + dofs + 16);
		h = PG_LE32 (lump + dofs + 20);
		blob = 40 + w*h + (w/2)*(h/2) + (w/4)*(h/4) + (w/8)*(h/8);
		if (dofs + blob > lumplen)
			continue;
		*out_len = blob;
		return dofs;
	}
	return 0;
}

// Build pg_texdata from the source map. Returns false if the map or any
// required texture is missing.
static qboolean PG_LoadTextures (void)
{
	byte		*bsp;
	const unsigned char *tl;
	int		tofs, tlen, i, ndata;
	int		hdrsize, blobcur;

	bsp = COM_LoadTempFile (PG_TEXSOURCE);
	if (!bsp || PG_LE32 (bsp) != PG_BSPVERSION)
		return false;

	tofs = PG_LE32 (bsp + 4 + PG_LUMP_TEXTURES * 8);
	tlen = PG_LE32 (bsp + 4 + PG_LUMP_TEXTURES * 8 + 4);
	tl = bsp + tofs;

	hdrsize = 4 + 4 * PG_NUMTEX;		// nummiptex + dataofs[]
	ndata = hdrsize;

	// first pass: lay out offsets and total size
	for (i = 0; i < PG_NUMTEX; i++)
	{
		int len, mofs = PG_FindMiptex (tl, tlen, pg_texpick[i].name, &len);
		if (!mofs)
			return false;
		ndata += len;
	}
	if (ndata > PG_MAX_TEXBYTES)
		return false;

	// second pass: emit the compact lump
	*(int *)(pg_texdata) = PG_NUMTEX;
	hdrsize = 4 + 4 * PG_NUMTEX;
	blobcur = hdrsize;
	for (i = 0; i < PG_NUMTEX; i++)
	{
		int len, mofs = PG_FindMiptex (tl, tlen, pg_texpick[i].name, &len);
		((int *)(pg_texdata + 4))[i] = blobcur;
		memcpy (pg_texdata + blobcur, tl + mofs, len);
		blobcur += len;
	}
	pg_texlen = blobcur;
	return true;
}

// Create the three axial texinfos for every texture and bucket them by role.
static void PG_SetupTexinfos (void)
{
	int	i;

	pg_num_wall = pg_num_floor = 0;
	pg_ceil_tex = 0;
	pg_portal_tex = pg_frame_tex = 0;
	for (i = 0; i < PG_NUMTEX; i++)
	{
		pg_tix[i] = PG_AddTexinfo (0,1,0,  0,0,-1, i);	// X-wall: s=+Y t=-Z
		pg_tiy[i] = PG_AddTexinfo (1,0,0,  0,0,-1, i);	// Y-wall: s=+X t=-Z
		pg_tiz[i] = PG_AddTexinfo (1,0,0,  0,1,0,  i);	// floor/ceiling
		switch (pg_texpick[i].role)
		{
		case PG_ROLE_WALL:   pg_wall_opt[pg_num_wall++] = i; break;
		case PG_ROLE_FLOOR:  pg_floor_opt[pg_num_floor++] = i; break;
		case PG_ROLE_CEIL:   pg_ceil_tex = i; break;
		case PG_ROLE_PORTAL: pg_portal_tex = i; break;
		case PG_ROLE_FRAME:  pg_frame_tex = i; break;
		}
	}
}

//=========================================================================
// Dungeon layout
//=========================================================================

static int PG_OpenCell (int cx, int cy)
{
	if (cx < 0 || cy < 0 || cx >= pg_nx || cy >= pg_ny)
		return 0;
	return pg_cellopen[cx][cy];
}

// Open a room cell and give it random textures.
static void PG_OpenRoom (int cx, int cy)
{
	pg_cellopen[cx][cy] = 1;
	pg_walltex[cx][cy]  = pg_wall_opt[pg_range (0, pg_num_wall - 1)];
	pg_floortex[cx][cy] = pg_floor_opt[pg_range (0, pg_num_floor - 1)];
}

// Choose the exit room (the open room farthest from the spawn) and a solid
// wall of it to host the slipgate. Records the room cell, the wall side, and
// the world-space centre of the gate for the proximity trigger.
static void PG_PlacePortal (void)
{
	int	cx, cy, best = -1, bestcx = pg_start_cx, bestcy = pg_start_cy;
	int	dx[4] = { -1, 1, 0, 0 };
	int	dy[4] = { 0, 0, -1, 1 };
	int	s;

	pg_have_portal = false;

	for (cx = 0; cx < pg_nx; cx += 2)
		for (cy = 0; cy < pg_ny; cy += 2)
		{
			int d;
			if (!pg_cellopen[cx][cy])
				continue;
			d = abs (cx - pg_start_cx) + abs (cy - pg_start_cy);
			if (d > best)
			{
				best = d;
				bestcx = cx;
				bestcy = cy;
			}
		}

	pg_portal_cx = bestcx;
	pg_portal_cy = bestcy;

	// pick a side whose neighbouring cell is solid (so the gate sits in a
	// real wall, not an open passage)
	for (s = 0; s < 4; s++)
		if (!PG_OpenCell (bestcx + dx[s], bestcy + dy[s]))
		{
			int x0 = pg_gx[bestcx], x1 = pg_gx[bestcx + 1];
			int y0 = pg_gy[bestcy], y1 = pg_gy[bestcy + 1];
			pg_portal_side = s;
			pg_have_portal = true;
			pg_portal_z = 56;
			switch (s)
			{
			case 0: pg_portal_x = x0; pg_portal_y = (y0 + y1) / 2; break;	// -X wall
			case 1: pg_portal_x = x1; pg_portal_y = (y0 + y1) / 2; break;	// +X wall
			case 2: pg_portal_x = (x0 + x1) / 2; pg_portal_y = y0; break;	// -Y wall
			case 3: pg_portal_x = (x0 + x1) / 2; pg_portal_y = y1; break;	// +Y wall
			}
			break;
		}
}

// Lay out an interleaved room/wall grid. A random walk over the rooms opens a
// connected set, carving the wall cell between each pair of rooms it steps
// across into a passage; everything else stays solid.
static void PG_Layout (void)
{
	int	gw, gh, rx, ry, steps, i, cx, cy;

	gw = pg_range (PG_ROOMS_MIN, PG_ROOMS_MAX);
	gh = pg_range (PG_ROOMS_MIN, PG_ROOMS_MAX);
	pg_nx = 2 * gw - 1;
	pg_ny = 2 * gh - 1;

	memset (pg_cellopen, 0, sizeof (pg_cellopen));

	// cell line coordinates: rooms get a random footprint, walls are thin.
	// Room sizes are multiples of 32 so every cell boundary stays 16-aligned;
	// that keeps surface extents == span (<=256) and avoids "Bad surface
	// extents" from texture-coordinate rounding.
	pg_gx[0] = 0;
	for (i = 0; i < pg_nx; i++)
		pg_gx[i + 1] = pg_gx[i] + ((i & 1) ? PG_WALLSZ : 32 * pg_range (PG_ROOMMIN / 32, PG_ROOMMAX / 32));
	pg_gy[0] = 0;
	for (i = 0; i < pg_ny; i++)
		pg_gy[i + 1] = pg_gy[i] + ((i & 1) ? PG_WALLSZ : 32 * pg_range (PG_ROOMMIN / 32, PG_ROOMMAX / 32));
	// centre the whole thing on the origin (offset stays a multiple of 16)
	for (i = 0; i <= pg_nx; i++) pg_gx[i] -= pg_gx[pg_nx] / 2;
	for (i = 0; i <= pg_ny; i++) pg_gy[i] -= pg_gy[pg_ny] / 2;

	rx = gw / 2;
	ry = gh / 2;
	pg_start_cx = 2 * rx;
	pg_start_cy = 2 * ry;
	PG_OpenRoom (2 * rx, 2 * ry);

	steps = gw * gh * 2;
	for (i = 0; i < steps; i++)
	{
		int dir = pg_range (0, 3);
		int nrx = rx, nry = ry;
		if (dir == 0 && rx > 0)      nrx--;
		else if (dir == 1 && rx < gw - 1) nrx++;
		else if (dir == 2 && ry > 0) nry--;
		else if (dir == 3 && ry < gh - 1) nry++;
		if (nrx == rx && nry == ry)
			continue;
		// open the wall cell between the two rooms, then the new room
		PG_OpenRoom (rx + nrx, ry + nry);	// midpoint cell (even+odd)
		PG_OpenRoom (2 * nrx, 2 * nry);
		rx = nrx;
		ry = nry;
	}

	// passages get a consistent texture so corridors read distinctly
	for (cx = 0; cx < pg_nx; cx++)
		for (cy = 0; cy < pg_ny; cy++)
			if (pg_cellopen[cx][cy] && ((cx & 1) || (cy & 1)))
			{
				pg_walltex[cx][cy]  = pg_wall_opt[pg_num_wall - 1];
				pg_floortex[cx][cy] = pg_floor_opt[0];
			}

	PG_PlacePortal ();
}

//=========================================================================
// Per-cell geometry
//=========================================================================

// Emit one axis-aligned wall sub-rectangle on the given side. (u, z) ranges
// select a patch of the wall; winding matches the full-wall quads so the
// visible normal points into the room.
static int PG_AddWallRect (int side, int planenum, int fixed,
			   int ua, int ub, int za, int zb, int ti)
{
	switch (side)
	{
	case 0:	// -X wall at x=fixed, normal +X, u=Y
		return PG_AddQuad (planenum, 0, ti,
			fixed,ua,za,  fixed,ub,za,  fixed,ub,zb,  fixed,ua,zb);
	case 1:	// +X wall at x=fixed, normal -X, u=Y
		return PG_AddQuad (planenum, 1, ti,
			fixed,ua,za,  fixed,ua,zb,  fixed,ub,zb,  fixed,ub,za);
	case 2:	// -Y wall at y=fixed, normal +Y, u=X
		return PG_AddQuad (planenum, 0, ti,
			ua,fixed,za,  ua,fixed,zb,  ub,fixed,zb,  ub,fixed,za);
	default:// +Y wall at y=fixed, normal -Y, u=X
		return PG_AddQuad (planenum, 1, ti,
			ua,fixed,za,  ub,fixed,za,  ub,fixed,zb,  ua,fixed,zb);
	}
}

// Emit a framed slipgate set into a wall: a centred warp panel surrounded by
// gate-frame faces. All faces are coplanar (same wall plane/side), so the
// generic node chain renders them as one flush wall. Returns the new face count.
static int PG_EmitPortalWall (int side, int planenum, int fixed,
			      int a0, int a1, int h,
			      int *fpl, int *fside, int *fidx, int n)
{
	int	uc = (a0 + a1) / 2, uL = uc - 32, uR = uc + 32;
	int	ztop = (h - 8 < 112) ? (h - 8) : 112;
	int	sideflag = (side == 0 || side == 2) ? 0 : 1;
	int	frameTI = (side < 2) ? pg_tix[pg_frame_tex]  : pg_tiy[pg_frame_tex];
	int	portTI  = (side < 2) ? pg_tix[pg_portal_tex] : pg_tiy[pg_portal_tex];

	fpl[n]=planenum; fside[n]=sideflag;	// frame: left of the gate
	fidx[n]=PG_AddWallRect (side, planenum, fixed, a0, uL, 0, h, frameTI); n++;
	fpl[n]=planenum; fside[n]=sideflag;	// frame: right of the gate
	fidx[n]=PG_AddWallRect (side, planenum, fixed, uR, a1, 0, h, frameTI); n++;
	fpl[n]=planenum; fside[n]=sideflag;	// frame: lintel above the gate
	fidx[n]=PG_AddWallRect (side, planenum, fixed, uL, uR, ztop, h, frameTI); n++;
	fpl[n]=planenum; fside[n]=sideflag;	// the warp surface itself
	fidx[n]=PG_AddWallRect (side, planenum, fixed, uL, uR, 0, ztop, portTI); n++;
	return n;
}

// Build the rendering subtree for one cell. Returns a BSP child code: a node
// index (>=0), or for a solid cell the shared solid leaf (<0). An open cell
// emits a wall only where its neighbour is solid, plus a floor and ceiling,
// then a node chain (interior side continues, the other side is solid) ending
// in an empty leaf that marks every face it contains.
static int PG_RenderCell (int cx, int cy)
{
	int	x0, y0, x1, y1, z0, z1;
	int	fpl[16], fside[16], fidx[16], n = 0;
	int	i, first, emptyleaf;
	int	tx, ty, tz, tc;		// texinfos for this cell
	int	is_portal;		// this is the exit room hosting the gate

	if (!PG_OpenCell (cx, cy))
		return -1 - pg_solidleaf;

	x0 = pg_gx[cx]; x1 = pg_gx[cx + 1];
	y0 = pg_gy[cy]; y1 = pg_gy[cy + 1];
	z0 = 0;         z1 = pg_height;
	tx = pg_tix[pg_walltex[cx][cy]];
	ty = pg_tiy[pg_walltex[cx][cy]];
	tz = pg_tiz[pg_floortex[cx][cy]];
	tc = pg_tiz[pg_ceil_tex];
	is_portal = (pg_have_portal && cx == pg_portal_cx && cy == pg_portal_cy);

	if (!PG_OpenCell (cx - 1, cy))		// -X wall (normal +X)
	{
		int pl = PG_AddPlane (1,0,0, x0, PG_PLANE_X);
		if (is_portal && pg_portal_side == 0)
			n = PG_EmitPortalWall (0, pl, x0, y0, y1, z1, fpl, fside, fidx, n);
		else { fpl[n]=pl; fside[n]=0; fidx[n]=PG_AddQuad (pl, 0, tx,
			x0,y0,z0,  x0,y1,z0,  x0,y1,z1,  x0,y0,z1); n++; }
	}
	if (!PG_OpenCell (cx + 1, cy))		// +X wall (normal -X)
	{
		int pl = PG_AddPlane (1,0,0, x1, PG_PLANE_X);
		if (is_portal && pg_portal_side == 1)
			n = PG_EmitPortalWall (1, pl, x1, y0, y1, z1, fpl, fside, fidx, n);
		else { fpl[n]=pl; fside[n]=1; fidx[n]=PG_AddQuad (pl, 1, tx,
			x1,y0,z0,  x1,y0,z1,  x1,y1,z1,  x1,y1,z0); n++; }
	}
	if (!PG_OpenCell (cx, cy - 1))		// -Y wall (normal +Y)
	{
		int pl = PG_AddPlane (0,1,0, y0, PG_PLANE_Y);
		if (is_portal && pg_portal_side == 2)
			n = PG_EmitPortalWall (2, pl, y0, x0, x1, z1, fpl, fside, fidx, n);
		else { fpl[n]=pl; fside[n]=0; fidx[n]=PG_AddQuad (pl, 0, ty,
			x0,y0,z0,  x0,y0,z1,  x1,y0,z1,  x1,y0,z0); n++; }
	}
	if (!PG_OpenCell (cx, cy + 1))		// +Y wall (normal -Y)
	{
		int pl = PG_AddPlane (0,1,0, y1, PG_PLANE_Y);
		if (is_portal && pg_portal_side == 3)
			n = PG_EmitPortalWall (3, pl, y1, x0, x1, z1, fpl, fside, fidx, n);
		else { fpl[n]=pl; fside[n]=1; fidx[n]=PG_AddQuad (pl, 1, ty,
			x0,y1,z0,  x1,y1,z0,  x1,y1,z1,  x0,y1,z1); n++; }
	}
	// floor (normal +Z)
	fpl[n] = PG_AddPlane (0,0,1, z0, PG_PLANE_Z); fside[n] = 0;
	fidx[n] = PG_AddQuad (fpl[n], 0, tz,
		x0,y0,z0,  x1,y0,z0,  x1,y1,z0,  x0,y1,z0); n++;
	// ceiling (normal -Z)
	fpl[n] = PG_AddPlane (0,0,1, z1, PG_PLANE_Z); fside[n] = 1;
	fidx[n] = PG_AddQuad (fpl[n], 1, tc,
		x0,y0,z1,  x0,y1,z1,  x1,y1,z1,  x1,y0,z1); n++;

	emptyleaf = pg.numleafs;
	{
		pg_leaf_t *l = &pg.leafs[pg.numleafs++];
		memset (l, 0, sizeof (*l));
		l->contents = PG_CONTENTS_EMPTY;
		l->visofs = -1;
		l->mins[0]=x0; l->mins[1]=y0; l->mins[2]=z0;
		l->maxs[0]=x1; l->maxs[1]=y1; l->maxs[2]=z1;
		l->firstmarksurface = pg.nummarksurf;
		l->nummarksurfaces = n;
		for (i = 0; i < n; i++)
			pg.marksurf[pg.nummarksurf++] = fidx[i];
	}

	first = pg.numnodes;
	for (i = 0; i < n; i++)
	{
		pg_node_t *nd = &pg.nodes[pg.numnodes++];
		int interior_is_front = (fside[i] == 0) ? 1 : 0;
		int next = (i < n - 1) ? (first + i + 1) : (-1 - emptyleaf);
		int solid = -1 - pg_solidleaf;
		nd->planenum = fpl[i];
		if (interior_is_front) { nd->children[0] = next; nd->children[1] = solid; }
		else                   { nd->children[0] = solid; nd->children[1] = next; }
		nd->mins[0]=x0; nd->mins[1]=y0; nd->mins[2]=z0;
		nd->maxs[0]=x1; nd->maxs[1]=y1; nd->maxs[2]=z1;
		nd->firstface = fidx[i];
		nd->numfaces = 1;
	}
	return first;
}

// Recursively partition a cell range into single cells with axial grid planes
// (a k-d tree). Each leaf cell becomes a PG_RenderCell subtree, so every leaf
// of the final BSP is one convex cell.
static int PG_RenderTree (int cx0, int cx1, int cy0, int cy1)
{
	int	nx = cx1 - cx0, ny = cy1 - cy0;
	int	node, mid, c0, c1;

	if (nx == 1 && ny == 1)
		return PG_RenderCell (cx0, cy0);

	node = pg.numnodes++;		// reserve before recursing
	if (nx >= ny)
	{
		mid = cx0 + nx / 2;
		pg.nodes[node].planenum = PG_AddPlane (1,0,0, pg_gx[mid], PG_PLANE_X);
		c0 = PG_RenderTree (mid, cx1, cy0, cy1);	// +X side is front
		c1 = PG_RenderTree (cx0, mid, cy0, cy1);
	}
	else
	{
		mid = cy0 + ny / 2;
		pg.nodes[node].planenum = PG_AddPlane (0,1,0, pg_gy[mid], PG_PLANE_Y);
		c0 = PG_RenderTree (cx0, cx1, mid, cy1);
		c1 = PG_RenderTree (cx0, cx1, cy0, mid);
	}
	pg.nodes[node].children[0] = c0;
	pg.nodes[node].children[1] = c1;
	pg.nodes[node].mins[0] = pg_gx[cx0]; pg.nodes[node].maxs[0] = pg_gx[cx1];
	pg.nodes[node].mins[1] = pg_gy[cy0]; pg.nodes[node].maxs[1] = pg_gy[cy1];
	pg.nodes[node].mins[2] = 0;          pg.nodes[node].maxs[2] = pg_height;
	pg.nodes[node].firstface = 0;
	pg.nodes[node].numfaces = 0;
	return node;
}

// Collision clip-hull subtree for one cell. Solid-adjacent walls become clip
// planes offset inward by the hull half-size; open boundaries get no plane so
// the player passes freely between cells. Returns a clipnode index (>=0) or a
// contents code (<0).
static int PG_ClipCell (int h, int cx, int cy)
{
	int	x0, y0, x1, y1, z0, z1;
	int	pl[6], side[6], n = 0;
	int	i, first;
	int	hxy = (h == 0) ? 16 : 32;	// player vs big-monster half width
	int	hzn = 24;			// origin sits 24 above the floor
	int	hzp = (h == 0) ? 32 : 64;	// head room below the ceiling

	if (!PG_OpenCell (cx, cy))
		return PG_CONTENTS_SOLID;

	x0 = pg_gx[cx]; x1 = pg_gx[cx + 1];
	y0 = pg_gy[cy]; y1 = pg_gy[cy + 1];
	z0 = 0;         z1 = pg_height;

	if (!PG_OpenCell (cx - 1, cy)) { pl[n] = PG_AddPlane (1,0,0, x0 + hxy, PG_PLANE_X); side[n] = 0; n++; }
	if (!PG_OpenCell (cx + 1, cy)) { pl[n] = PG_AddPlane (1,0,0, x1 - hxy, PG_PLANE_X); side[n] = 1; n++; }
	if (!PG_OpenCell (cx, cy - 1)) { pl[n] = PG_AddPlane (0,1,0, y0 + hxy, PG_PLANE_Y); side[n] = 0; n++; }
	if (!PG_OpenCell (cx, cy + 1)) { pl[n] = PG_AddPlane (0,1,0, y1 - hxy, PG_PLANE_Y); side[n] = 1; n++; }
	pl[n] = PG_AddPlane (0,0,1, z0 + hzn, PG_PLANE_Z); side[n] = 0; n++;	// floor
	pl[n] = PG_AddPlane (0,0,1, z1 - hzp, PG_PLANE_Z); side[n] = 1; n++;	// ceiling

	first = pg.numclipnodes;
	for (i = 0; i < n; i++)
	{
		pg_clipnode_t *cn = &pg.clipnodes[pg.numclipnodes++];
		int interior_is_front = (side[i] == 0) ? 1 : 0;
		int next = (i < n - 1) ? (first + i + 1) : PG_CONTENTS_EMPTY;
		cn->planenum = pl[i];
		if (interior_is_front) { cn->children[0] = next; cn->children[1] = PG_CONTENTS_SOLID; }
		else                   { cn->children[0] = PG_CONTENTS_SOLID; cn->children[1] = next; }
	}
	return first;
}

static int PG_ClipTree (int h, int cx0, int cx1, int cy0, int cy1)
{
	int	nx = cx1 - cx0, ny = cy1 - cy0;
	int	node, mid, c0, c1;

	if (nx == 1 && ny == 1)
		return PG_ClipCell (h, cx0, cy0);

	node = pg.numclipnodes++;	// reserve before recursing
	if (nx >= ny)
	{
		mid = cx0 + nx / 2;
		pg.clipnodes[node].planenum = PG_AddPlane (1,0,0, pg_gx[mid], PG_PLANE_X);
		c0 = PG_ClipTree (h, mid, cx1, cy0, cy1);
		c1 = PG_ClipTree (h, cx0, mid, cy0, cy1);
	}
	else
	{
		mid = cy0 + ny / 2;
		pg.clipnodes[node].planenum = PG_AddPlane (0,1,0, pg_gy[mid], PG_PLANE_Y);
		c0 = PG_ClipTree (h, cx0, cx1, mid, cy1);
		c1 = PG_ClipTree (h, cx0, cx1, cy0, mid);
	}
	pg.clipnodes[node].children[0] = c0;
	pg.clipnodes[node].children[1] = c1;
	return node;
}

// Assemble the whole dungeon: the required solid leaf, then the rendering BSP
// and both collision hulls over the cell grid.
static void PG_BuildDungeon (void)
{
	pg_height = pg_range (PG_HMIN, PG_HMAX);

	pg_solidleaf = pg.numleafs;			// leaf 0 must be solid
	{
		pg_leaf_t *l = &pg.leafs[pg.numleafs++];
		memset (l, 0, sizeof (*l));
		l->contents = PG_CONTENTS_SOLID;
		l->visofs = -1;
	}

	pg_render_root = PG_RenderTree (0, pg_nx, 0, pg_ny);
	pg_hull1_root  = PG_ClipTree (0, 0, pg_nx, 0, pg_ny);
	pg_hull2_root  = PG_ClipTree (1, 0, pg_nx, 0, pg_ny);
}

//=========================================================================
// Doors
//
// A door is a separate BSP brush model (referenced by a func_door entity as
// "*N"). It fills one open passage cell from floor to ceiling and, when
// triggered, slides straight up (angle -1, lip 0) so it clears completely into
// the solid space above the ceiling. The world clip hulls leave that passage
// open, so the door alone gates it.
//=========================================================================

// Six outward-facing box faces. The six box planes (positive unit normals,
// indexed -X,+X,-Y,+Y,-Z,+Z) are written back into bpl[] so the collision hull
// can reuse them. Returns the index of the first face.
static int PG_AddDoorFaces (int x0, int y0, int x1, int y1, int z0, int z1,
			    int tex, int *bpl)
{
	int	first = pg.numfaces;
	int	tx = pg_tix[tex], ty = pg_tiy[tex], tz = pg_tiz[tex];

	bpl[0] = PG_AddPlane (1,0,0, x0, PG_PLANE_X);	// -X face (visible from -X)
	PG_AddQuad (bpl[0], 1, tx, x0,y0,z0,  x0,y0,z1,  x0,y1,z1,  x0,y1,z0);
	bpl[1] = PG_AddPlane (1,0,0, x1, PG_PLANE_X);	// +X face
	PG_AddQuad (bpl[1], 0, tx, x1,y0,z0,  x1,y1,z0,  x1,y1,z1,  x1,y0,z1);
	bpl[2] = PG_AddPlane (0,1,0, y0, PG_PLANE_Y);	// -Y face
	PG_AddQuad (bpl[2], 1, ty, x0,y0,z0,  x1,y0,z0,  x1,y0,z1,  x0,y0,z1);
	bpl[3] = PG_AddPlane (0,1,0, y1, PG_PLANE_Y);	// +Y face
	PG_AddQuad (bpl[3], 0, ty, x0,y1,z0,  x0,y1,z1,  x1,y1,z1,  x1,y1,z0);
	bpl[4] = PG_AddPlane (0,0,1, z0, PG_PLANE_Z);	// -Z face (bottom)
	PG_AddQuad (bpl[4], 1, tz, x0,y0,z0,  x0,y1,z0,  x1,y1,z0,  x1,y0,z0);
	bpl[5] = PG_AddPlane (0,0,1, z1, PG_PLANE_Z);	// +Z face (top)
	PG_AddQuad (bpl[5], 0, tz, x0,y0,z1,  x1,y0,z1,  x1,y1,z1,  x0,y1,z1);
	return first;
}

// Render/point-collision hull (hull 0): a node box that is the solid leaf
// inside the brush and the shared empty leaf outside. Returns the root node.
static int PG_AddDoorHull0 (const int *bpl,
			    int x0, int y0, int x1, int y1, int z0, int z1)
{
	int	first = pg.numnodes;
	int	emp = -1 - pg_door_emptyleaf;
	int	sol = -1 - pg_solidleaf;
	int	i;
	// per plane: which child is "inside the box" (continue), and the front/back
	struct { int front, back; } ch[6];

	ch[0].front = first + 1; ch[0].back  = emp;	// x > x0 -> inside
	ch[1].front = emp;       ch[1].back  = first + 2;	// x < x1 -> inside
	ch[2].front = first + 3; ch[2].back  = emp;	// y > y0
	ch[3].front = emp;       ch[3].back  = first + 4;	// y < y1
	ch[4].front = first + 5; ch[4].back  = emp;	// z > z0
	ch[5].front = emp;       ch[5].back  = sol;	// z < z1 -> solid

	for (i = 0; i < 6; i++)
	{
		pg_node_t *nd = &pg.nodes[pg.numnodes++];
		memset (nd, 0, sizeof (*nd));
		nd->planenum = bpl[i];
		nd->children[0] = ch[i].front;
		nd->children[1] = ch[i].back;
		nd->mins[0]=x0; nd->mins[1]=y0; nd->mins[2]=z0;
		nd->maxs[0]=x1; nd->maxs[1]=y1; nd->maxs[2]=z1;
		nd->firstface = 0;
		nd->numfaces = 0;
	}
	return first;
}

// Collision clip hull (hull 1 or 2): a clipnode box that is SOLID inside the
// brush (expanded outward by the standard player/monster box) and EMPTY
// outside, matching how qbsp expands stock door brushes. Returns the root.
static int PG_AddDoorClip (int h, int x0, int y0, int x1, int y1, int z0, int z1)
{
	int	hxy = (h == 0) ? 16 : 32;	// player vs big-monster half width
	int	topexp = 24;			// origin sits 24 above the floor
	int	botexp = (h == 0) ? 32 : 64;	// head room below the ceiling
	int	first = pg.numclipnodes;
	int	i;
	struct { float nx, ny, nz, d; int type, front, back; } pl[6];

	pl[0].nx=1;pl[0].ny=0;pl[0].nz=0; pl[0].d=x0-hxy;   pl[0].type=PG_PLANE_X;
	pl[0].front = first + 1;       pl[0].back  = PG_CONTENTS_EMPTY;
	pl[1].nx=1;pl[1].ny=0;pl[1].nz=0; pl[1].d=x1+hxy;   pl[1].type=PG_PLANE_X;
	pl[1].front = PG_CONTENTS_EMPTY; pl[1].back = first + 2;
	pl[2].nx=0;pl[2].ny=1;pl[2].nz=0; pl[2].d=y0-hxy;   pl[2].type=PG_PLANE_Y;
	pl[2].front = first + 3;       pl[2].back  = PG_CONTENTS_EMPTY;
	pl[3].nx=0;pl[3].ny=1;pl[3].nz=0; pl[3].d=y1+hxy;   pl[3].type=PG_PLANE_Y;
	pl[3].front = PG_CONTENTS_EMPTY; pl[3].back = first + 4;
	pl[4].nx=0;pl[4].ny=0;pl[4].nz=1; pl[4].d=z0-botexp; pl[4].type=PG_PLANE_Z;
	pl[4].front = first + 5;       pl[4].back  = PG_CONTENTS_EMPTY;
	pl[5].nx=0;pl[5].ny=0;pl[5].nz=1; pl[5].d=z1+topexp; pl[5].type=PG_PLANE_Z;
	pl[5].front = PG_CONTENTS_EMPTY; pl[5].back = PG_CONTENTS_SOLID;

	for (i = 0; i < 6; i++)
	{
		pg_clipnode_t *cn = &pg.clipnodes[pg.numclipnodes++];
		cn->planenum = PG_AddPlane (pl[i].nx, pl[i].ny, pl[i].nz, pl[i].d, pl[i].type);
		cn->children[0] = pl[i].front;
		cn->children[1] = pl[i].back;
	}
	return first;
}

// Build all of a door's geometry and fill its submodel record.
static void PG_EmitDoor (int idx)
{
	pg_door_t	*d = &pg_doors[idx];
	pg_model_t	*m = &pg_doormodels[idx];
	int		bpl[6];
	int		x0 = pg_gx[d->cx], x1 = pg_gx[d->cx + 1];
	int		y0 = pg_gy[d->cy], y1 = pg_gy[d->cy + 1];
	int		z0 = 0, z1 = pg_height;
	int		ff;

	if (pg_door_emptyleaf < 0)		// create the shared exterior leaf once
	{
		pg_leaf_t *l = &pg.leafs[pg.numleafs++];
		memset (l, 0, sizeof (*l));
		l->contents = PG_CONTENTS_EMPTY;
		l->visofs = -1;
		pg_door_emptyleaf = pg.numleafs - 1;
	}

	ff = PG_AddDoorFaces (x0, y0, x1, y1, z0, z1, pg_frame_tex, bpl);

	memset (m, 0, sizeof (*m));
	m->mins[0]=x0; m->mins[1]=y0; m->mins[2]=z0;
	m->maxs[0]=x1; m->maxs[1]=y1; m->maxs[2]=z1;
	m->headnode[0] = PG_AddDoorHull0 (bpl, x0, y0, x1, y1, z0, z1);
	m->headnode[1] = PG_AddDoorClip (0, x0, y0, x1, y1, z0, z1);
	m->headnode[2] = PG_AddDoorClip (1, x0, y0, x1, y1, z0, z1);
	m->headnode[3] = 0;
	m->visleafs = 0;
	m->firstface = ff;
	m->numfaces = 6;

	d->submodel = idx + 1;			// model 0 is the world
}

// Flood the open rooms reachable from spawn without crossing a locked passage.
static void PG_ReachableRooms (unsigned char vis[PG_NCELL][PG_NCELL])
{
	int	stack[PG_NCELL * PG_NCELL][2];
	int	sp = 0;
	int	dx[4] = { -2, 2, 0, 0 };
	int	dy[4] = { 0, 0, -2, 2 };

	memset (vis, 0, PG_NCELL * PG_NCELL);
	vis[pg_start_cx][pg_start_cy] = 1;
	stack[sp][0] = pg_start_cx; stack[sp][1] = pg_start_cy; sp++;

	while (sp > 0)
	{
		int cx, cy, i;
		sp--;
		cx = stack[sp][0]; cy = stack[sp][1];
		for (i = 0; i < 4; i++)
		{
			int nx = cx + dx[i], ny = cy + dy[i];
			int mx = cx + dx[i] / 2, my = cy + dy[i] / 2;	// passage cell
			if (nx < 0 || ny < 0 || nx >= pg_nx || ny >= pg_ny)
				continue;
			if (!pg_cellopen[mx][my] || pg_locked[mx][my])
				continue;		// wall or gated by a key we lack
			if (!pg_cellopen[nx][ny] || vis[nx][ny])
				continue;
			vis[nx][ny] = 1;
			stack[sp][0] = nx; stack[sp][1] = ny; sp++;
		}
	}
}

// Decide which passages become doors, guarantee the keys stay reachable, and
// build every door's brush submodel. Call after PG_BuildDungeon.
static void PG_BuildDoors (void)
{
	unsigned char	vis[PG_NCELL][PG_NCELL];
	int		cx, cy, i;
	int		krx[PG_NCELL * PG_NCELL], kry[PG_NCELL * PG_NCELL], nk;

	pg_ndoors = 0;
	pg_door_emptyleaf = -1;
	pg_need_silver = pg_need_gold = false;
	memset (pg_locked, 0, sizeof (pg_locked));

	// Decide door placement first so locked passages are known before routing.
	// Roughly 40% of passages get a door; the rest stay plain framed openings.
	for (cx = 0; cx < pg_nx && pg_ndoors < PG_MAXDOORS; cx++)
		for (cy = 0; cy < pg_ny && pg_ndoors < PG_MAXDOORS; cy++)
		{
			if (!pg_cellopen[cx][cy])
				continue;
			if (((cx & 1) != 0) == ((cy & 1) != 0))
				continue;		// not a passage (room or corner)
			if (pg_range (0, 9) >= 4)
				continue;		// ~60% stay plain framed openings
			// Skip a passage whose door would share a room corner with an
			// already-placed door. Two such door brushes touch at that corner,
			// and stock LinkDoors would link them into one group (objerror'ing
			// "cross connected doors" when three meet). Keeping every door
			// isolated lets each spawn its own trigger field and open on touch.
			{
				int	j, adj = 0;
				for (j = 0; j < pg_ndoors; j++)
					if (abs (pg_doors[j].cx - cx) == 1 &&
					    abs (pg_doors[j].cy - cy) == 1)
						{ adj = 1; break; }
				if (adj)
					continue;
			}
			pg_doors[pg_ndoors].cx = cx;
			pg_doors[pg_ndoors].cy = cy;
			pg_doors[pg_ndoors].type = PG_DOOR_AUTO;
			pg_ndoors++;
		}

	// Promote up to one silver- and one gold-locked door (randomized per map),
	// so some doors gate areas behind a key without ever softlocking the map.
	if (pg_ndoors > 0 && pg_range (0, 1))
	{
		i = pg_range (0, pg_ndoors - 1);
		pg_doors[i].type = PG_DOOR_SILVER;
		pg_locked[pg_doors[i].cx][pg_doors[i].cy] = 1;
		pg_need_silver = true;
	}
	if (pg_ndoors > 1 && pg_range (0, 1))
	{
		for (i = 0; i < 8; i++)		// a few tries to land on an auto door
		{
			int j = pg_range (0, pg_ndoors - 1);
			if (pg_doors[j].type == PG_DOOR_AUTO)
			{
				pg_doors[j].type = PG_DOOR_GOLD;
				pg_locked[pg_doors[j].cx][pg_doors[j].cy] = 1;
				pg_need_gold = true;
				break;
			}
		}
	}

	// Keys must sit in a room reachable from spawn without crossing any locked
	// door, so every dungeon stays clearable.
	PG_ReachableRooms (vis);
	nk = 0;
	for (cx = 0; cx < pg_nx; cx++)
		for (cy = 0; cy < pg_ny; cy++)
			if (vis[cx][cy] && !((cx & 1) || (cy & 1)) &&
			    !(cx == pg_start_cx && cy == pg_start_cy))
			{
				krx[nk] = (pg_gx[cx] + pg_gx[cx + 1]) / 2;
				kry[nk] = (pg_gy[cy] + pg_gy[cy + 1]) / 2;
				nk++;
			}

	pg_silverkey_x = pg_goldkey_x = (pg_gx[pg_start_cx] + pg_gx[pg_start_cx + 1]) / 2;
	pg_silverkey_y = pg_goldkey_y = (pg_gy[pg_start_cy] + pg_gy[pg_start_cy + 1]) / 2;
	if (nk > 0)
	{
		if (pg_need_silver)
		{
			int r = pg_range (0, nk - 1);
			pg_silverkey_x = krx[r]; pg_silverkey_y = kry[r];
		}
		if (pg_need_gold)
		{
			int r = pg_range (0, nk - 1);
			pg_goldkey_x = krx[r]; pg_goldkey_y = kry[r];
		}
	}

	for (i = 0; i < pg_ndoors; i++)
		PG_EmitDoor (i);
}

//=========================================================================
// Serialization
//=========================================================================

static int PG_Align4 (int x) { return (x + 3) & ~3; }

// Append a lump's bytes to buf at the running offset, recording the header.
static int PG_PutLump (unsigned char *buf, int ofs, pg_lump_t *lump,
		       const void *data, int len)
{
	ofs = PG_Align4 (ofs);
	lump->fileofs = ofs;
	lump->filelen = len;
	if (len)
		memcpy (buf + ofs, data, len);
	return ofs + len;
}

// Scatter monsters and items through the open rooms (skipping the spawn room).
// The player is armed in the spawn room and every level is seeded with enough
// ammo to clear all of its monsters, so a dungeon can always be finished.
static void PG_PlaceEntities (void)
{
	// Zombies are intentionally excluded: they only die to explosive gibbing,
	// so with the guaranteed shotgun/nailgun loadout a zombie could never be
	// killed, leaving the level uncleared and the slipgate shut forever.
	static const char *monsters[] = {
		"monster_army", "monster_dog", "monster_ogre",
		"monster_knight", "monster_wizard",
	};
	static const int mhealth[] = { 30, 25, 200, 75, 80 };
	char	line[256];
	int	cx, cy, i;
	int	rx[256], ry[256], nrooms = 0;
	int	threat = 0;
	int	sx, sy;

	// collect every non-spawn room centre
	for (cx = 0; cx < pg_nx; cx++)
		for (cy = 0; cy < pg_ny; cy++)
		{
			if (!pg_cellopen[cx][cy])
				continue;
			if ((cx & 1) || (cy & 1))	// rooms only, not passages
				continue;
			if (cx == pg_start_cx && cy == pg_start_cy)
				continue;
			if (nrooms < 256)
			{
				rx[nrooms] = (pg_gx[cx] + pg_gx[cx + 1]) / 2;
				ry[nrooms] = (pg_gy[cy] + pg_gy[cy + 1]) / 2;
				nrooms++;
			}
		}

	// arm the player right where they spawn
	sx = (pg_gx[pg_start_cx] + pg_gx[pg_start_cx + 1]) / 2;
	sy = (pg_gy[pg_start_cy] + pg_gy[pg_start_cy + 1]) / 2;
	sprintf (line, "{\n\"classname\" \"weapon_supershotgun\"\n\"origin\" \"%i %i %i\"\n}\n",
		sx - 40, sy, 24);
	PG_EntCat (line);
	sprintf (line, "{\n\"classname\" \"weapon_nailgun\"\n\"origin\" \"%i %i %i\"\n}\n",
		sx + 40, sy, 24);
	PG_EntCat (line);

	// monsters in ~2/3 of rooms, with the odd health/armour pickup
	for (i = 0; i < nrooms; i++)
	{
		if (pg_range (0, 2))
		{
			int mi = pg_range (0, 4);
			sprintf (line,
				"{\n\"classname\" \"%s\"\n\"origin\" \"%i %i %i\"\n\"angle\" \"%i\"\n}\n",
				monsters[mi], rx[i], ry[i], 40, pg_range (0, 3) * 90);
			PG_EntCat (line);
			threat += mhealth[mi];
		}
		if (pg_range (0, 2) == 0)
		{
			const char *it = pg_range (0, 1) ? "item_health" : "item_armor1";
			sprintf (line, "{\n\"classname\" \"%s\"\n\"origin\" \"%i %i %i\"\n}\n",
				it, rx[i] + 24, ry[i] + 24, 24);
			PG_EntCat (line);
		}
	}

	// Seed enough ammo to clear the level. Big shell/nail boxes carry ~40
	// shells (~24 dmg each via the super shotgun) and ~50 nails (~9 dmg each);
	// keep dropping them across the rooms until their damage potential covers
	// every monster three times over (margin for misses; the player also keeps
	// the starting shotgun + 25 shells). A floor guarantees a stash even on
	// near-empty maps.
	{
		int need = threat * 3 + 200;
		int got = 0;
		int slot = 0;

		// a starter box at the spawn point so the player is stocked at once
		sprintf (line,
			"{\n\"classname\" \"item_shells\"\n\"spawnflags\" \"1\"\n\"origin\" \"%i %i %i\"\n}\n",
			sx, sy + 48, 24);
		PG_EntCat (line);
		got += 40 * 24;

		while (got < need && nrooms > 0 && slot < 256)
		{
			int r = slot % nrooms;
			if (slot & 1)
			{
				sprintf (line,
					"{\n\"classname\" \"item_spikes\"\n\"spawnflags\" \"1\"\n\"origin\" \"%i %i %i\"\n}\n",
					rx[r] - 28, ry[r] + 28, 24);
				got += 50 * 9;
			}
			else
			{
				sprintf (line,
					"{\n\"classname\" \"item_shells\"\n\"spawnflags\" \"1\"\n\"origin\" \"%i %i %i\"\n}\n",
					rx[r] - 28, ry[r] - 28, 24);
				got += 40 * 24;
			}
			PG_EntCat (line);
			slot++;
		}
	}
}

// Emit the func_door entities (referencing their brush submodels) and any keys
// the locked doors need. Keys were already placed in a reachable room.
static void PG_PlaceDoors (void)
{
	char	line[256];
	int	i;

	for (i = 0; i < pg_ndoors; i++)
	{
		// No DOOR_DONT_LINK: that flag makes LinkDoors skip spawning the door's
		// trigger field, so a plain (non-key) door could never be opened by
		// walking up to it. Door placement already guarantees no two door
		// brushes share a room corner, so LinkDoors links each door only to
		// itself and gives it its own auto-open field. 16 = silver, 8 = gold key.
		int spawnflags = 0;
		if (pg_doors[i].type == PG_DOOR_SILVER) spawnflags |= 16;
		else if (pg_doors[i].type == PG_DOOR_GOLD) spawnflags |= 8;
		// angle -1 = slide straight up; lip 0 = clear fully into the ceiling
		sprintf (line,
			"{\n\"classname\" \"func_door\"\n\"model\" \"*%i\"\n"
			"\"angle\" \"-1\"\n\"lip\" \"0\"\n\"speed\" \"140\"\n\"wait\" \"4\"\n"
			"\"spawnflags\" \"%i\"\n}\n",
			pg_doors[i].submodel, spawnflags);
		PG_EntCat (line);
	}

	if (pg_need_silver)
	{
		sprintf (line, "{\n\"classname\" \"item_key1\"\n\"origin\" \"%i %i %i\"\n}\n",
			pg_silverkey_x, pg_silverkey_y, 24);
		PG_EntCat (line);
	}
	if (pg_need_gold)
	{
		sprintf (line, "{\n\"classname\" \"item_key2\"\n\"origin\" \"%i %i %i\"\n}\n",
			pg_goldkey_x, pg_goldkey_y, 24);
		PG_EntCat (line);
	}
}

// Build the entity string and return the finished BSP image. Caller frees.
static unsigned char *PG_Serialize (int *out_len, int px, int py, int pz)
{
	pg_model_t	models[1 + PG_MAXDOORS];
	pg_model_t	*model = &models[0];
	char		line[256];
	unsigned char	*buf;
	pg_header_t	*hdr;
	int		ofs, i, nmodels;

	// entities
	pg.entlen = 0;
	pg.entities[0] = 0;
	PG_EntCat ("{\n\"classname\" \"worldspawn\"\n\"message\" \"Procedural Dungeon\"\n\"worldtype\" \"0\"\n}\n");
	sprintf (line, "{\n\"classname\" \"info_player_start\"\n\"origin\" \"%i %i %i\"\n\"angle\" \"90\"\n}\n", px, py, pz);
	PG_EntCat (line);
	PG_PlaceEntities ();
	PG_PlaceDoors ();

	// world model (0) followed by one brush submodel per door
	memset (model, 0, sizeof (*model));
	model->mins[0]=-4096; model->mins[1]=-4096; model->mins[2]=-4096;
	model->maxs[0]= 4096; model->maxs[1]= 4096; model->maxs[2]= 4096;
	model->headnode[0] = pg_render_root;	// render root node
	model->headnode[1] = pg_hull1_root;	// hull1 clipnode root
	model->headnode[2] = pg_hull2_root;	// hull2 clipnode root
	model->headnode[3] = 0;
	model->visleafs = pg_worldleafs - 1;	// excluding solid leaf 0, excluding doors
	model->firstface = 0;
	model->numfaces = pg_worldfaces;	// world faces only; door faces follow

	for (i = 0; i < pg_ndoors; i++)
		models[1 + i] = pg_doormodels[i];
	nmodels = 1 + pg_ndoors;

	*out_len = 0;
	buf = malloc (PG_MAPBUF_CAP);
	if (!buf)
		return NULL;
	memset (buf, 0, PG_MAPBUF_CAP);
	hdr = (pg_header_t *)buf;
	hdr->version = PG_BSPVERSION;

	ofs = sizeof (pg_header_t);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_ENTITIES], pg.entities, pg.entlen + 1);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_PLANES], pg.planes, pg.numplanes * sizeof (pg_plane_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_TEXTURES], pg_texdata, pg_texlen);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_VERTEXES], pg.verts, pg.numverts * sizeof (pg_vertex_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_VISIBILITY], NULL, 0);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_NODES], pg.nodes, pg.numnodes * sizeof (pg_node_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_TEXINFO], pg.texinfos, pg.numtexinfo * sizeof (pg_texinfo_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_FACES], pg.faces, pg.numfaces * sizeof (pg_face_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_LIGHTING], NULL, 0);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_CLIPNODES], pg.clipnodes, pg.numclipnodes * sizeof (pg_clipnode_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_LEAFS], pg.leafs, pg.numleafs * sizeof (pg_leaf_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_MARKSURFACES], pg.marksurf, pg.nummarksurf * sizeof (unsigned short));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_EDGES], pg.edges, pg.numedges * sizeof (pg_edge_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_SURFEDGES], pg.surfedges, pg.numsurfedges * sizeof (int));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_MODELS], models, nmodels * sizeof (pg_model_t));

	*out_len = ofs;
	return buf;
}

//=========================================================================
// Command
//=========================================================================

static void PG_Reset (void)
{
	memset (&pg, 0, sizeof (pg));
	// edge 0 is reserved (negative surfedge indices can't reference it)
	pg.numedges = 1;
}

static qboolean PG_WriteFile (const char *path, unsigned char *data, int len)
{
	FILE *fp;

	mkdir ("id1", 0777);
	mkdir ("id1/maps", 0777);
	fp = fopen (path, "wb");
	if (!fp)
		return false;
	fwrite (data, 1, len, fp);
	fclose (fp);
	return true;
}

// Generate a dungeon for the given seed and write it to maps/procgen.bsp.
// Returns the room count, or -1 on failure.
static int PG_Generate (unsigned int seed)
{
	int		px, py, pz, cx, cy, rooms, len;
	unsigned char	*bsp;

	pg_seed = seed;
	pg_last_seed = seed;

	PG_Reset ();
	if (!PG_LoadTextures ())
	{
		Con_Printf ("procgen: could not load textures from %s\n", PG_TEXSOURCE);
		return -1;
	}
	PG_SetupTexinfos ();
	PG_Layout ();
	PG_BuildDungeon ();
	pg_worldfaces = pg.numfaces;	// door geometry is appended after the world
	pg_worldleafs = pg.numleafs;
	PG_BuildDoors ();

	// spawn in the middle of the starting cell
	px = (pg_gx[pg_start_cx] + pg_gx[pg_start_cx + 1]) / 2;
	py = (pg_gy[pg_start_cy] + pg_gy[pg_start_cy + 1]) / 2;
	pz = 40;

	bsp = PG_Serialize (&len, px, py, pz);
	if (!bsp)
	{
		Con_Printf ("procgen: out of memory\n");
		return -1;
	}
	if (!PG_WriteFile ("id1/maps/procgen.bsp", bsp, len))
	{
		Con_Printf ("procgen: could not write map file\n");
		free (bsp);
		return -1;
	}
	free (bsp);

	pg_portal_open = false;		// freshly generated: gate dormant again
	pg_portal_used = false;
	pg_portal_announced = false;

	rooms = 0;
	for (cy = 0; cy < pg_ny; cy += 2)
		for (cx = 0; cx < pg_nx; cx += 2)
			rooms += pg_cellopen[cx][cy];
	return rooms;
}

void Procgen_f (void)
{
	unsigned int	seed;
	int		rooms;

	// COM_FindFile refuses on-disk paths containing '/' under the shareware
	// data (a licensing guard). Our generated map lives at maps/procgen.bsp,
	// so allow directory-tree lookups for it.
	extern int static_registered;
	static_registered = 1;

	if (Cmd_Argc () >= 2)
		seed = (unsigned int)Q_atoi (Cmd_Argv (1));
	else
		seed = (unsigned int)(Sys_FloatTime () * 1000.0);

	rooms = PG_Generate (seed);
	if (rooms < 0)
		return;

	Con_Printf ("procgen: built %i-room dungeon (seed %u), loading...\n",
		rooms, seed);
	Cbuf_AddText ("map procgen\n");
}

// Triggered when the player steps into an opened slipgate: roll a fresh dungeon
// and change to it, carrying weapons/health across like a real level exit.
static void Procgen_Next_f (void)
{
	extern int static_registered;
	int rooms;

	static_registered = 1;
	rooms = PG_Generate ((unsigned int)(Sys_FloatTime () * 1000.0) ^ (pg_seed * 2654435761u));
	if (rooms < 0)
		return;
	Con_Printf ("procgen: slipgate to a new %i-room dungeon...\n", rooms);
	Cbuf_AddText ("changelevel procgen\n");
}

// Called every server frame. Once the level is cleared the slipgate "opens"
// (announced to the player); stepping into it warps to a new random dungeon.
void Procgen_PortalThink (void)
{
	edict_t	*pl;
	float	dx, dy, dz;

	if (!sv.active || pg_seed == 0)
		return;
	if (strcmp (sv.name, "procgen") != 0)
		return;			// not one of our maps
	if (!pg_have_portal || pg_portal_used)
		return;

	if (!pg_portal_open)
	{
		int total = (int)pr_global_struct->total_monsters;
		int killed = (int)pr_global_struct->killed_monsters;
		if (total <= 0 || killed >= total)
			pg_portal_open = true;
		else
			return;
	}

	pl = svs.clients[0].edict;
	if (!svs.clients[0].active || !pl || pl->free)
		return;

	if (!pg_portal_announced)
	{
		client_t *cl = &svs.clients[0];
		MSG_WriteByte (&cl->message, svc_centerprint);
		MSG_WriteString (&cl->message,
			"The slipgate roars to life!\nStep through to escape.");
		pg_portal_announced = true;
	}

	dx = pl->v.origin[0] - pg_portal_x;
	dy = pl->v.origin[1] - pg_portal_y;
	dz = pl->v.origin[2] - pg_portal_z;
	if (dx*dx + dy*dy < 56.0f*56.0f && dz > -64.0f && dz < 96.0f)
	{
		pg_portal_used = true;		// guard against re-triggering
		Cbuf_AddText ("procgen_next\n");
	}
}

void Procgen_Init (void)
{
	Cmd_AddCommand ("procgen", Procgen_f);
	Cmd_AddCommand ("procgen_next", Procgen_Next_f);
}

// --- Web/JS bridges: is the current level a procgen dungeon, and its seed? ---
EMSCRIPTEN_KEEPALIVE
int Web_IsProcgen (void)
{
	return (sv.active && strcmp (sv.name, "procgen") == 0) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
unsigned int Web_ProcgenSeed (void)
{
	return pg_last_seed;
}

// Serialize the current dungeon's layout for the JS minimap overlay. The buffer
// is a flat int32 array drawn straight from the generator's cell grid (the same
// data the BSP was built from), so the map reflects the real geometry:
//   [0] = total ints written
//   [1..4] = world bounds (minx, miny, maxx, maxy)
//   [5..6] = spawn centre (x, y)
//   [7]    = portal present (0/1)
//   [8..9] = portal world centre (x, y)
//   then one (x0,y0,x1,y1) rect per open (walkable) cell.
static int pg_mapbuf[16 + PG_NCELL * PG_NCELL * 4];

EMSCRIPTEN_KEEPALIVE
int *Web_ProcgenMap (void)
{
	int	cx, cy, n;
	const int cap = (int)(sizeof (pg_mapbuf) / sizeof (pg_mapbuf[0]));

	if (pg_nx <= 0 || pg_ny <= 0)
	{
		pg_mapbuf[0] = 0;
		return pg_mapbuf;
	}

	pg_mapbuf[1] = pg_gx[0];
	pg_mapbuf[2] = pg_gy[0];
	pg_mapbuf[3] = pg_gx[pg_nx];
	pg_mapbuf[4] = pg_gy[pg_ny];
	pg_mapbuf[5] = (pg_gx[pg_start_cx] + pg_gx[pg_start_cx + 1]) / 2;
	pg_mapbuf[6] = (pg_gy[pg_start_cy] + pg_gy[pg_start_cy + 1]) / 2;
	pg_mapbuf[7] = pg_have_portal ? 1 : 0;
	pg_mapbuf[8] = pg_portal_x;
	pg_mapbuf[9] = pg_portal_y;

	n = 10;
	for (cy = 0; cy < pg_ny; cy++)
		for (cx = 0; cx < pg_nx; cx++)
		{
			if (!pg_cellopen[cx][cy])
				continue;
			if (n + 4 > cap)
				goto done;
			pg_mapbuf[n++] = pg_gx[cx];
			pg_mapbuf[n++] = pg_gy[cy];
			pg_mapbuf[n++] = pg_gx[cx + 1];
			pg_mapbuf[n++] = pg_gy[cy + 1];
		}
done:
	pg_mapbuf[0] = n;
	return pg_mapbuf;
}
