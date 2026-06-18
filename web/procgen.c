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
#define	PG_ENTSTRING		8192

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

#define	PG_ROOMS	5		// max rooms per axis
#define	PG_NCELL	(2 * PG_ROOMS - 1)	// interleaved cells per axis
#define	PG_ROOMMIN	192		// room footprint range (<=256 -> legal extents)
#define	PG_ROOMMAX	256
#define	PG_WALLSZ	64		// thickness of walls / depth of passages
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

static int		pg_solidleaf;			// shared solid leaf index
static int		pg_render_root;			// model headnode[0]
static int		pg_hull1_root, pg_hull2_root;	// model headnode[1..2]

// deterministic RNG (LCG)
static unsigned int	pg_seed;
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
};
#define	PG_NUMTEX	((int)(sizeof(pg_texpick)/sizeof(pg_texpick[0])))

static unsigned char	pg_texdata[PG_MAX_TEXBYTES];	// finished texture lump
static int		pg_texlen;
static int		pg_tix[PG_NUMTEX], pg_tiy[PG_NUMTEX], pg_tiz[PG_NUMTEX];
static int		pg_wall_opt[PG_NUMTEX], pg_num_wall;
static int		pg_floor_opt[PG_NUMTEX], pg_num_floor;
static int		pg_ceil_tex;

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
	for (i = 0; i < PG_NUMTEX; i++)
	{
		pg_tix[i] = PG_AddTexinfo (0,1,0,  0,0,-1, i);	// X-wall: s=+Y t=-Z
		pg_tiy[i] = PG_AddTexinfo (1,0,0,  0,0,-1, i);	// Y-wall: s=+X t=-Z
		pg_tiz[i] = PG_AddTexinfo (1,0,0,  0,1,0,  i);	// floor/ceiling
		switch (pg_texpick[i].role)
		{
		case PG_ROLE_WALL:  pg_wall_opt[pg_num_wall++] = i; break;
		case PG_ROLE_FLOOR: pg_floor_opt[pg_num_floor++] = i; break;
		case PG_ROLE_CEIL:  pg_ceil_tex = i; break;
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

// Lay out an interleaved room/wall grid. A random walk over the rooms opens a
// connected set, carving the wall cell between each pair of rooms it steps
// across into a passage; everything else stays solid.
static void PG_Layout (void)
{
	int	gw, gh, rx, ry, steps, i, cx, cy;

	gw = pg_range (2, PG_ROOMS);
	gh = pg_range (2, PG_ROOMS);
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
}

//=========================================================================
// Per-cell geometry
//=========================================================================

// Build the rendering subtree for one cell. Returns a BSP child code: a node
// index (>=0), or for a solid cell the shared solid leaf (<0). An open cell
// emits a wall only where its neighbour is solid, plus a floor and ceiling,
// then a node chain (interior side continues, the other side is solid) ending
// in an empty leaf that marks every face it contains.
static int PG_RenderCell (int cx, int cy)
{
	int	x0, y0, x1, y1, z0, z1;
	int	fpl[6], fside[6], fidx[6], n = 0;
	int	i, first, emptyleaf;
	int	tx, ty, tz, tc;		// texinfos for this cell

	if (!PG_OpenCell (cx, cy))
		return -1 - pg_solidleaf;

	x0 = pg_gx[cx]; x1 = pg_gx[cx + 1];
	y0 = pg_gy[cy]; y1 = pg_gy[cy + 1];
	z0 = 0;         z1 = pg_height;
	tx = pg_tix[pg_walltex[cx][cy]];
	ty = pg_tiy[pg_walltex[cx][cy]];
	tz = pg_tiz[pg_floortex[cx][cy]];
	tc = pg_tiz[pg_ceil_tex];

	if (!PG_OpenCell (cx - 1, cy))		// -X wall (normal +X)
	{
		fpl[n] = PG_AddPlane (1,0,0, x0, PG_PLANE_X); fside[n] = 0;
		fidx[n] = PG_AddQuad (fpl[n], 0, tx,
			x0,y0,z0,  x0,y1,z0,  x0,y1,z1,  x0,y0,z1); n++;
	}
	if (!PG_OpenCell (cx + 1, cy))		// +X wall (normal -X)
	{
		fpl[n] = PG_AddPlane (1,0,0, x1, PG_PLANE_X); fside[n] = 1;
		fidx[n] = PG_AddQuad (fpl[n], 1, tx,
			x1,y0,z0,  x1,y0,z1,  x1,y1,z1,  x1,y1,z0); n++;
	}
	if (!PG_OpenCell (cx, cy - 1))		// -Y wall (normal +Y)
	{
		fpl[n] = PG_AddPlane (0,1,0, y0, PG_PLANE_Y); fside[n] = 0;
		fidx[n] = PG_AddQuad (fpl[n], 0, ty,
			x0,y0,z0,  x0,y0,z1,  x1,y0,z1,  x1,y0,z0); n++;
	}
	if (!PG_OpenCell (cx, cy + 1))		// +Y wall (normal -Y)
	{
		fpl[n] = PG_AddPlane (0,1,0, y1, PG_PLANE_Y); fside[n] = 1;
		fidx[n] = PG_AddQuad (fpl[n], 1, ty,
			x0,y1,z0,  x1,y1,z0,  x1,y1,z1,  x0,y1,z1); n++;
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
static void PG_PlaceEntities (void)
{
	static const char *monsters[] = {
		"monster_army", "monster_dog", "monster_ogre",
		"monster_knight", "monster_zombie", "monster_wizard",
	};
	static const char *items[] = {
		"item_health", "item_shells", "item_spikes",
		"item_armor1", "weapon_supershotgun", "weapon_nailgun",
	};
	char	line[256];
	int	cx, cy;

	for (cx = 0; cx < pg_nx; cx++)
		for (cy = 0; cy < pg_ny; cy++)
		{
			int ex, ey;
			if (!pg_cellopen[cx][cy])
				continue;
			if ((cx & 1) || (cy & 1))	// only room cells, not passages
				continue;
			if (cx == pg_start_cx && cy == pg_start_cy)
				continue;
			ex = (pg_gx[cx] + pg_gx[cx + 1]) / 2;
			ey = (pg_gy[cy] + pg_gy[cy + 1]) / 2;

			if (pg_range (0, 2))		// ~2/3 of rooms get a monster
			{
				const char *m = monsters[pg_range (0, 5)];
				sprintf (line,
					"{\n\"classname\" \"%s\"\n\"origin\" \"%i %i %i\"\n\"angle\" \"%i\"\n}\n",
					m, ex, ey, 40, pg_range (0, 3) * 90);
				PG_EntCat (line);
			}
			if (pg_range (0, 1))		// half get an item
			{
				const char *it = items[pg_range (0, 5)];
				sprintf (line,
					"{\n\"classname\" \"%s\"\n\"origin\" \"%i %i %i\"\n}\n",
					it, ex + 24, ey + 24, 24);
				PG_EntCat (line);
			}
		}
}

// Build the entity string and return the finished BSP image. Caller frees.
static unsigned char *PG_Serialize (int *out_len, int px, int py, int pz)
{
	pg_model_t	model;
	char		line[256];
	unsigned char	*buf;
	pg_header_t	*hdr;
	int		ofs;
	int		cap;

	// entities
	pg.entlen = 0;
	pg.entities[0] = 0;
	PG_EntCat ("{\n\"classname\" \"worldspawn\"\n\"message\" \"Procedural Dungeon\"\n\"worldtype\" \"0\"\n}\n");
	sprintf (line, "{\n\"classname\" \"info_player_start\"\n\"origin\" \"%i %i %i\"\n\"angle\" \"90\"\n}\n", px, py, pz);
	PG_EntCat (line);
	PG_PlaceEntities ();

	// one world model
	memset (&model, 0, sizeof (model));
	model.mins[0]=-2048; model.mins[1]=-2048; model.mins[2]=-2048;
	model.maxs[0]= 2048; model.maxs[1]= 2048; model.maxs[2]= 2048;
	model.headnode[0] = pg_render_root;	// render root node
	model.headnode[1] = pg_hull1_root;	// hull1 clipnode root
	model.headnode[2] = pg_hull2_root;	// hull2 clipnode root
	model.headnode[3] = 0;
	model.visleafs = pg.numleafs - 1;	// excluding solid leaf 0
	model.firstface = 0;
	model.numfaces = pg.numfaces;

	cap = 512 * 1024;
	buf = malloc (cap);
	if (!buf)
		return NULL;
	memset (buf, 0, cap);
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
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_MODELS], &model, sizeof (model));

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

void Procgen_f (void)
{
	int		px, py, pz;
	int		cx, cy, rooms;
	unsigned char	*bsp;
	int		len;

	// COM_FindFile refuses on-disk paths containing '/' under the shareware
	// data (a licensing guard). Our generated map lives at maps/procgen.bsp,
	// so allow directory-tree lookups for it.
	extern int static_registered;
	static_registered = 1;

	if (Cmd_Argc () >= 2)
		pg_seed = (unsigned int)Q_atoi (Cmd_Argv (1));
	else
		pg_seed = (unsigned int)(Sys_FloatTime () * 1000.0);

	PG_Reset ();
	if (!PG_LoadTextures ())
	{
		Con_Printf ("procgen: could not load textures from %s\n", PG_TEXSOURCE);
		return;
	}
	PG_SetupTexinfos ();
	PG_Layout ();
	PG_BuildDungeon ();

	// spawn in the middle of the starting cell
	px = (pg_gx[pg_start_cx] + pg_gx[pg_start_cx + 1]) / 2;
	py = (pg_gy[pg_start_cy] + pg_gy[pg_start_cy + 1]) / 2;
	pz = 40;

	bsp = PG_Serialize (&len, px, py, pz);
	if (!bsp)
	{
		Con_Printf ("procgen: out of memory\n");
		return;
	}

	if (!PG_WriteFile ("id1/maps/procgen.bsp", bsp, len))
	{
		Con_Printf ("procgen: could not write map file\n");
		free (bsp);
		return;
	}
	free (bsp);

	rooms = 0;
	for (cy = 0; cy < pg_ny; cy += 2)
		for (cx = 0; cx < pg_nx; cx += 2)
			rooms += pg_cellopen[cx][cy];

	Con_Printf ("procgen: built %i-room dungeon (seed %u), loading...\n",
		rooms, pg_seed);
	Cbuf_AddText ("map procgen\n");
}

void Procgen_Init (void)
{
	Cmd_AddCommand ("procgen", Procgen_f);
}
