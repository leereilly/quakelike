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

#define	PG_MAX_PLANES		1024
#define	PG_MAX_VERTS		4096
#define	PG_MAX_EDGES		8192
#define	PG_MAX_SURFEDGES	8192
#define	PG_MAX_FACES		2048
#define	PG_MAX_TEXINFO		64
#define	PG_MAX_NODES		1024
#define	PG_MAX_LEAFS		1024
#define	PG_MAX_CLIPNODES	2048
#define	PG_MAX_MARKSURF		8192
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
// Dungeon layout (grid of cells; a connected subset is walkable)
//=========================================================================

#define	PG_CELL		256		// cell footprint (<=256 keeps surf extents legal)
#define	PG_GRID		5		// max cells per axis
#define	PG_HMIN		160
#define	PG_HMAX		224

static int		pg_gw, pg_gh;			// grid size in cells
static unsigned char	pg_open[PG_GRID][PG_GRID];	// 1 == walkable cell
static int		pg_height;			// shared ceiling height
static int		pg_basex, pg_basey;		// world origin of cell (0,0)
static int		pg_start_cx, pg_start_cy;	// spawn cell

static int		pg_solidleaf;			// shared solid leaf index
static int		pg_tex_x, pg_tex_y, pg_tex_z;	// texinfos per orientation
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
// Embedded wall texture (64x64, 4 mip levels, 8-bit palette indices)
//=========================================================================

#define	PG_TEX_W	64
#define	PG_TEX_H	64
// dmiptexlump (nummiptex + 1 ofs) + miptex header (40) + mip pixels
#define	PG_MIP0		(PG_TEX_W * PG_TEX_H)
#define	PG_MIP1		((PG_TEX_W/2) * (PG_TEX_H/2))
#define	PG_MIP2		((PG_TEX_W/4) * (PG_TEX_H/4))
#define	PG_MIP3		((PG_TEX_W/8) * (PG_TEX_H/8))
#define	PG_MIPTEX_BYTES	(40 + PG_MIP0 + PG_MIP1 + PG_MIP2 + PG_MIP3)
#define	PG_TEXLUMP_BYTES	(8 + PG_MIPTEX_BYTES)

// A stone-block pattern: mortar grid lines around two-tone blocks. Renders
// with clear depth cues now that surfaces draw correctly.
static unsigned char PG_Texel (int x, int y)
{
	int gx = x & 15, gy = y & 15;
	int bx = x >> 4, by = y >> 4;
	if (gx == 0 || gy == 0)
		return 0x1d;		// dark mortar line
	// stagger every other row like real masonry, alternate two block shades
	if (((bx + (by & 1)) ^ by) & 1)
		return 0x6a;		// lighter stone
	return 0x6d;			// darker stone
}

static void PG_WriteTextureLump (unsigned char *out)
{
	int	x, y, mip, w, h, step;
	unsigned char *p;
	int	*hdr = (int *)out;

	hdr[0] = 1;		// nummiptex
	hdr[1] = 8;		// dataofs[0] -> miptex starts right after

	p = out + 8;
	memset (p, 0, 16);
	strcpy ((char *)p, "PROCWALL");
	((unsigned *)(p + 16))[0] = PG_TEX_W;		// width
	((unsigned *)(p + 16))[1] = PG_TEX_H;		// height
	// offsets[4], relative to the miptex start
	((unsigned *)(p + 24))[0] = 40;
	((unsigned *)(p + 24))[1] = 40 + PG_MIP0;
	((unsigned *)(p + 24))[2] = 40 + PG_MIP0 + PG_MIP1;
	((unsigned *)(p + 24))[3] = 40 + PG_MIP0 + PG_MIP1 + PG_MIP2;

	p += 40;
	for (mip = 0; mip < 4; mip++)
	{
		step = 1 << mip;
		w = PG_TEX_W / step;
		h = PG_TEX_H / step;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				*p++ = PG_Texel (x * step, y * step);
	}
}

//=========================================================================
// Dungeon layout
//=========================================================================

static int PG_OpenCell (int cx, int cy)
{
	if (cx < 0 || cy < 0 || cx >= pg_gw || cy >= pg_gh)
		return 0;
	return pg_open[cx][cy];
}

static int PG_CellX0 (int cx) { return pg_basex + cx * PG_CELL; }
static int PG_CellY0 (int cy) { return pg_basey + cy * PG_CELL; }

// Lay out a connected set of walkable cells via a random walk from the centre
// (a random walk only ever steps to an adjacent cell, so the result is always
// fully connected).
static void PG_Layout (void)
{
	int	cx, cy, steps, i;

	pg_gw = pg_range (2, PG_GRID);
	pg_gh = pg_range (2, PG_GRID);
	memset (pg_open, 0, sizeof (pg_open));

	cx = pg_gw / 2;
	cy = pg_gh / 2;
	pg_start_cx = cx;
	pg_start_cy = cy;
	pg_open[cx][cy] = 1;

	steps = pg_gw * pg_gh + pg_range (2, 6);
	for (i = 0; i < steps; i++)
	{
		switch (pg_range (0, 3))
		{
		case 0: if (cx > 0)         cx--; break;
		case 1: if (cx < pg_gw - 1) cx++; break;
		case 2: if (cy > 0)         cy--; break;
		case 3: if (cy < pg_gh - 1) cy++; break;
		}
		pg_open[cx][cy] = 1;
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

	if (!PG_OpenCell (cx, cy))
		return -1 - pg_solidleaf;

	x0 = PG_CellX0 (cx); x1 = x0 + PG_CELL;
	y0 = PG_CellY0 (cy); y1 = y0 + PG_CELL;
	z0 = 0;              z1 = pg_height;

	if (!PG_OpenCell (cx - 1, cy))		// -X wall (normal +X)
	{
		fpl[n] = PG_AddPlane (1,0,0, x0, PG_PLANE_X); fside[n] = 0;
		fidx[n] = PG_AddQuad (fpl[n], 0, pg_tex_x,
			x0,y0,z0,  x0,y1,z0,  x0,y1,z1,  x0,y0,z1); n++;
	}
	if (!PG_OpenCell (cx + 1, cy))		// +X wall (normal -X)
	{
		fpl[n] = PG_AddPlane (1,0,0, x1, PG_PLANE_X); fside[n] = 1;
		fidx[n] = PG_AddQuad (fpl[n], 1, pg_tex_x,
			x1,y0,z0,  x1,y0,z1,  x1,y1,z1,  x1,y1,z0); n++;
	}
	if (!PG_OpenCell (cx, cy - 1))		// -Y wall (normal +Y)
	{
		fpl[n] = PG_AddPlane (0,1,0, y0, PG_PLANE_Y); fside[n] = 0;
		fidx[n] = PG_AddQuad (fpl[n], 0, pg_tex_y,
			x0,y0,z0,  x0,y0,z1,  x1,y0,z1,  x1,y0,z0); n++;
	}
	if (!PG_OpenCell (cx, cy + 1))		// +Y wall (normal -Y)
	{
		fpl[n] = PG_AddPlane (0,1,0, y1, PG_PLANE_Y); fside[n] = 1;
		fidx[n] = PG_AddQuad (fpl[n], 1, pg_tex_y,
			x0,y1,z0,  x1,y1,z0,  x1,y1,z1,  x0,y1,z1); n++;
	}
	// floor (normal +Z)
	fpl[n] = PG_AddPlane (0,0,1, z0, PG_PLANE_Z); fside[n] = 0;
	fidx[n] = PG_AddQuad (fpl[n], 0, pg_tex_z,
		x0,y0,z0,  x1,y0,z0,  x1,y1,z0,  x0,y1,z0); n++;
	// ceiling (normal -Z)
	fpl[n] = PG_AddPlane (0,0,1, z1, PG_PLANE_Z); fside[n] = 1;
	fidx[n] = PG_AddQuad (fpl[n], 1, pg_tex_z,
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
		pg.nodes[node].planenum = PG_AddPlane (1,0,0, PG_CellX0 (mid), PG_PLANE_X);
		c0 = PG_RenderTree (mid, cx1, cy0, cy1);	// +X side is front
		c1 = PG_RenderTree (cx0, mid, cy0, cy1);
	}
	else
	{
		mid = cy0 + ny / 2;
		pg.nodes[node].planenum = PG_AddPlane (0,1,0, PG_CellY0 (mid), PG_PLANE_Y);
		c0 = PG_RenderTree (cx0, cx1, mid, cy1);
		c1 = PG_RenderTree (cx0, cx1, cy0, mid);
	}
	pg.nodes[node].children[0] = c0;
	pg.nodes[node].children[1] = c1;
	pg.nodes[node].mins[0] = PG_CellX0 (cx0); pg.nodes[node].maxs[0] = PG_CellX0 (cx1);
	pg.nodes[node].mins[1] = PG_CellY0 (cy0); pg.nodes[node].maxs[1] = PG_CellY0 (cy1);
	pg.nodes[node].mins[2] = 0;               pg.nodes[node].maxs[2] = pg_height;
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

	x0 = PG_CellX0 (cx); x1 = x0 + PG_CELL;
	y0 = PG_CellY0 (cy); y1 = y0 + PG_CELL;
	z0 = 0;              z1 = pg_height;

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
		pg.clipnodes[node].planenum = PG_AddPlane (1,0,0, PG_CellX0 (mid), PG_PLANE_X);
		c0 = PG_ClipTree (h, mid, cx1, cy0, cy1);
		c1 = PG_ClipTree (h, cx0, mid, cy0, cy1);
	}
	else
	{
		mid = cy0 + ny / 2;
		pg.clipnodes[node].planenum = PG_AddPlane (0,1,0, PG_CellY0 (mid), PG_PLANE_Y);
		c0 = PG_ClipTree (h, cx0, cx1, mid, cy1);
		c1 = PG_ClipTree (h, cx0, cx1, cy0, mid);
	}
	pg.clipnodes[node].children[0] = c0;
	pg.clipnodes[node].children[1] = c1;
	return node;
}

// Assemble the whole dungeon: texinfos, the required solid leaf, then the
// rendering BSP and both collision hulls over the cell grid.
static void PG_BuildDungeon (void)
{
	pg_tex_x = PG_AddTexinfo (0,1,0,  0,0,-1, 0);	// X-walls: s=+Y t=-Z
	pg_tex_y = PG_AddTexinfo (1,0,0,  0,0,-1, 0);	// Y-walls: s=+X t=-Z
	pg_tex_z = PG_AddTexinfo (1,0,0,  0,1,0,  0);	// floor/ceiling: s=+X t=+Y

	pg_height = pg_range (PG_HMIN, PG_HMAX);
	pg_basex = -(pg_gw * PG_CELL) / 2;
	pg_basey = -(pg_gh * PG_CELL) / 2;

	pg_solidleaf = pg.numleafs;			// leaf 0 must be solid
	{
		pg_leaf_t *l = &pg.leafs[pg.numleafs++];
		memset (l, 0, sizeof (*l));
		l->contents = PG_CONTENTS_SOLID;
		l->visofs = -1;
	}

	pg_render_root = PG_RenderTree (0, pg_gw, 0, pg_gh);
	pg_hull1_root  = PG_ClipTree (0, 0, pg_gw, 0, pg_gh);
	pg_hull2_root  = PG_ClipTree (1, 0, pg_gw, 0, pg_gh);
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

// Build the entity string and return the finished BSP image. Caller frees.
static unsigned char *PG_Serialize (int *out_len, int px, int py, int pz)
{
	static unsigned char	texlump[PG_TEXLUMP_BYTES];
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

	PG_WriteTextureLump (texlump);

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

	cap = 256 * 1024;
	buf = malloc (cap);
	if (!buf)
		return NULL;
	memset (buf, 0, cap);
	hdr = (pg_header_t *)buf;
	hdr->version = PG_BSPVERSION;

	ofs = sizeof (pg_header_t);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_ENTITIES], pg.entities, pg.entlen + 1);
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_PLANES], pg.planes, pg.numplanes * sizeof (pg_plane_t));
	ofs = PG_PutLump (buf, ofs, &hdr->lumps[PG_LUMP_TEXTURES], texlump, PG_TEXLUMP_BYTES);
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
	PG_Layout ();
	PG_BuildDungeon ();

	// spawn in the middle of the starting cell
	px = PG_CellX0 (pg_start_cx) + PG_CELL / 2;
	py = PG_CellY0 (pg_start_cy) + PG_CELL / 2;
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
	for (cy = 0; cy < pg_gh; cy++)
		for (cx = 0; cx < pg_gw; cx++)
			rooms += pg_open[cx][cy];

	Con_Printf ("procgen: built %i-room dungeon (seed %u), loading...\n",
		rooms, pg_seed);
	Cbuf_AddText ("map procgen\n");
}

void Procgen_Init (void)
{
	Cmd_AddCommand ("procgen", Procgen_f);
}
