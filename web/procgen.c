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
// v1 generates a single randomized room. The data model and helpers are sized
// for many rooms so this can grow into full dungeons.

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
// Room geometry
//=========================================================================

// Build one axis-aligned empty box room. Walls face inward; the interior is a
// single empty leaf, everything else solid. Adds rendering nodes plus collision
// clipnodes for player (hull1) and big-monster (hull2) sizes.
static void PG_BuildRoom (int x0, int y0, int z0, int x1, int y1, int z1)
{
	int	tex_x, tex_y, tex_z;	// texinfos per wall orientation
	int	solidleaf, emptyleaf;
	int	rp[6];			// render plane indices
	int	v[8];
	int	i, f;
	int	face_verts[6][4];
	int	face_plane[6] = {0, 1, 2, 3, 4, 5};
	int	face_side[6]  = {0, 1, 0, 1, 0, 1};
	int	face_tex[6];
	int	firstnode;

	// --- texinfos (1 unit == 1 texel) ---
	tex_x = PG_AddTexinfo (0,1,0,  0,0,-1, 0);	// X-walls: s=+Y t=-Z
	tex_y = PG_AddTexinfo (1,0,0,  0,0,-1, 0);	// Y-walls: s=+X t=-Z
	tex_z = PG_AddTexinfo (1,0,0,  0,1,0,  0);	// floor/ceiling: s=+X t=+Y
	face_tex[0] = face_tex[1] = tex_x;
	face_tex[2] = face_tex[3] = tex_y;
	face_tex[4] = face_tex[5] = tex_z;

	// --- 8 corners, index = xi*4 + yi*2 + zi ---
	for (i = 0; i < 8; i++)
	{
		int xi = (i >> 2) & 1, yi = (i >> 1) & 1, zi = i & 1;
		v[i] = PG_AddVertex (xi ? x1 : x0, yi ? y1 : y0, zi ? z1 : z0);
	}
#define	VC(xi,yi,zi)	v[((xi)<<2)|((yi)<<1)|(zi)]

	// --- render planes (canonical positive axial normals) ---
	rp[0] = PG_AddPlane (1,0,0, x0, PG_PLANE_X);	// min X, interior front
	rp[1] = PG_AddPlane (1,0,0, x1, PG_PLANE_X);	// max X, interior back
	rp[2] = PG_AddPlane (0,1,0, y0, PG_PLANE_Y);
	rp[3] = PG_AddPlane (0,1,0, y1, PG_PLANE_Y);
	rp[4] = PG_AddPlane (0,0,1, z0, PG_PLANE_Z);
	rp[5] = PG_AddPlane (0,0,1, z1, PG_PLANE_Z);

	// --- face windings (CCW around the inward-facing normal) ---
	// min X (normal +X)
	face_verts[0][0]=VC(0,0,0); face_verts[0][1]=VC(0,1,0); face_verts[0][2]=VC(0,1,1); face_verts[0][3]=VC(0,0,1);
	// max X (normal -X)
	face_verts[1][0]=VC(1,0,0); face_verts[1][1]=VC(1,0,1); face_verts[1][2]=VC(1,1,1); face_verts[1][3]=VC(1,1,0);
	// min Y (normal +Y)
	face_verts[2][0]=VC(0,0,0); face_verts[2][1]=VC(0,0,1); face_verts[2][2]=VC(1,0,1); face_verts[2][3]=VC(1,0,0);
	// max Y (normal -Y)
	face_verts[3][0]=VC(0,1,0); face_verts[3][1]=VC(1,1,0); face_verts[3][2]=VC(1,1,1); face_verts[3][3]=VC(0,1,1);
	// min Z floor (normal +Z)
	face_verts[4][0]=VC(0,0,0); face_verts[4][1]=VC(1,0,0); face_verts[4][2]=VC(1,1,0); face_verts[4][3]=VC(0,1,0);
	// max Z ceiling (normal -Z)
	face_verts[5][0]=VC(0,0,1); face_verts[5][1]=VC(0,1,1); face_verts[5][2]=VC(1,1,1); face_verts[5][3]=VC(1,0,1);

	for (i = 0; i < 6; i++)
		f = PG_AddFace (rp[face_plane[i]], face_side[i], face_tex[i], face_verts[i], 4);
	(void)f;

	// --- leafs: 0 = solid (required), 1 = the room interior ---
	solidleaf = pg.numleafs;
	{
		pg_leaf_t *l = &pg.leafs[pg.numleafs++];
		memset (l, 0, sizeof (*l));
		l->contents = PG_CONTENTS_SOLID;
		l->visofs = -1;
	}
	emptyleaf = pg.numleafs;
	{
		pg_leaf_t *l = &pg.leafs[pg.numleafs++];
		memset (l, 0, sizeof (*l));
		l->contents = PG_CONTENTS_EMPTY;
		l->visofs = -1;
		l->mins[0]=x0; l->mins[1]=y0; l->mins[2]=z0;
		l->maxs[0]=x1; l->maxs[1]=y1; l->maxs[2]=z1;
		l->firstmarksurface = pg.nummarksurf;
		l->nummarksurfaces = 6;
		for (i = 0; i < 6; i++)
			pg.marksurf[pg.nummarksurf++] = i;
	}

	// --- rendering nodes: a chain of the 6 walls ---
	// interior child continues the chain; the other side is solid.
	firstnode = pg.numnodes;
	for (i = 0; i < 6; i++)
	{
		pg_node_t *nd = &pg.nodes[pg.numnodes++];
		int interior_is_front = (i & 1) ? 0 : 1;	// planes 0,2,4 front
		int next = (i < 5) ? (firstnode + i + 1) : (-1 - emptyleaf);
		int solid = -1 - solidleaf;
		nd->planenum = rp[i];
		if (interior_is_front) { nd->children[0] = next; nd->children[1] = solid; }
		else                   { nd->children[0] = solid; nd->children[1] = next; }
		nd->mins[0]=x0; nd->mins[1]=y0; nd->mins[2]=z0;
		nd->maxs[0]=x1; nd->maxs[1]=y1; nd->maxs[2]=z1;
		nd->firstface = i;	// the wall face that lies on this plane
		nd->numfaces = 1;
	}

	// --- collision clipnodes for hull1 (player) and hull2 (big) ---
	{
		// expansion offsets baked into the engine's hull defs
		static const int h1min[3] = {-16,-16,-24}, h1max[3] = {16,16,32};
		static const int h2min[3] = {-32,-32,-24}, h2max[3] = {32,32,64};
		int hb[2][6];	// per-hull expanded dists: x0,x1,y0,y1,z0,z1
		int h;

		hb[0][0]=x0 - h1min[0]; hb[0][1]=x1 - h1max[0];
		hb[0][2]=y0 - h1min[1]; hb[0][3]=y1 - h1max[1];
		hb[0][4]=z0 - h1min[2]; hb[0][5]=z1 - h1max[2];
		hb[1][0]=x0 - h2min[0]; hb[1][1]=x1 - h2max[0];
		hb[1][2]=y0 - h2min[1]; hb[1][3]=y1 - h2max[1];
		hb[1][4]=z0 - h2min[2]; hb[1][5]=z1 - h2max[2];

		for (h = 0; h < 2; h++)
		{
			int cp[6];
			int base = pg.numclipnodes;
			cp[0] = PG_AddPlane (1,0,0, hb[h][0], PG_PLANE_X);
			cp[1] = PG_AddPlane (1,0,0, hb[h][1], PG_PLANE_X);
			cp[2] = PG_AddPlane (0,1,0, hb[h][2], PG_PLANE_Y);
			cp[3] = PG_AddPlane (0,1,0, hb[h][3], PG_PLANE_Y);
			cp[4] = PG_AddPlane (0,0,1, hb[h][4], PG_PLANE_Z);
			cp[5] = PG_AddPlane (0,0,1, hb[h][5], PG_PLANE_Z);
			for (i = 0; i < 6; i++)
			{
				pg_clipnode_t *cn = &pg.clipnodes[pg.numclipnodes++];
				int interior_is_front = (i & 1) ? 0 : 1;
				int next = (i < 5) ? (base + i + 1) : PG_CONTENTS_EMPTY;
				cn->planenum = cp[i];
				if (interior_is_front) { cn->children[0]=next; cn->children[1]=PG_CONTENTS_SOLID; }
				else                   { cn->children[0]=PG_CONTENTS_SOLID; cn->children[1]=next; }
			}
		}
	}
#undef VC
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
	PG_EntCat ("{\n\"classname\" \"worldspawn\"\n\"message\" \"Procedural Room\"\n\"worldtype\" \"0\"\n}\n");
	sprintf (line, "{\n\"classname\" \"info_player_start\"\n\"origin\" \"%i %i %i\"\n\"angle\" \"90\"\n}\n", px, py, pz);
	PG_EntCat (line);

	PG_WriteTextureLump (texlump);

	// one world model
	memset (&model, 0, sizeof (model));
	model.mins[0]=-2048; model.mins[1]=-2048; model.mins[2]=-2048;
	model.maxs[0]= 2048; model.maxs[1]= 2048; model.maxs[2]= 2048;
	model.headnode[0] = 0;		// render root node
	model.headnode[1] = 0;		// hull1 clipnode root
	model.headnode[2] = 6;		// hull2 clipnode root (after hull1's 6)
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
	int		dimx, dimy, dimz;
	int		x0, y0, z0, x1, y1, z1;
	int		px, py, pz;
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

	// a single randomized room. Faces stay <= 256 units so software surface
	// extents remain valid.
	dimx = pg_range (192, 256);
	dimy = pg_range (192, 256);
	dimz = pg_range (160, 224);

	x0 = -dimx / 2; x1 = x0 + dimx;
	y0 = -dimy / 2; y1 = y0 + dimy;
	z0 = 0;         z1 = z0 + dimz;

	PG_BuildRoom (x0, y0, z0, x1, y1, z1);

	px = (x0 + x1) / 2;
	py = (y0 + y1) / 2;
	pz = z0 + 40;

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

	Con_Printf ("procgen: built %ix%ix%i room (seed %u), loading...\n",
		dimx, dimy, dimz, pg_seed);
	Cbuf_AddText ("map procgen\n");
}

void Procgen_Init (void)
{
	Cmd_AddCommand ("procgen", Procgen_f);
}
