/*
Copyright (C) 1997-2001 Id Software, Inc.

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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include "r_local.h"
#include "../qcommon/q_perf.h"
#include "../other/q_arena.h"

// the renderer is linked with the engine: read the BSP lumps directly
int		FS_FOpenFile (char *filename, FILE **file);
void	FS_Read (void *buffer, int len, FILE *f);
void	FS_ReadAt (FILE *h, int base, int ofs, void *buffer, int len);

static int Mod_AliasSize (dmdl_t *pinmodel);
static FILE	*mod_file;		// BSP being loaded
static int	mod_filebase;

// ESP32: planes and visibility data shared with the collision map
struct cplane_s *CM_SharedPlanes (char *name, int *num);
dvis_t	*CM_SharedVis (char *name);
static mplane_t	*shared_planes;
static int		shared_numplanes;
static dvis_t	*shared_vis;

model_t	*loadmodel;
char	loadname[32];	// for hunk tags

void Mod_LoadSpriteModel (model_t *mod, void *buffer);
void Mod_LoadBrushModel (model_t *mod, void *buffer);
void Mod_LoadAliasModel (model_t *mod, void *buffer);
model_t *Mod_LoadModel (model_t *mod, qboolean crash);

byte	mod_novis[MAX_MAP_LEAFS/8] EXT_RAM_BSS;

#define	MAX_MOD_KNOWN	160	// ESP32: 256 originally
#define	MAX_MOD_INLINE	200	// ESP32: inline models of the map (the most is 184)
model_t	mod_known[MAX_MOD_KNOWN] EXT_RAM_BSS;
int		mod_numknown;

// the inline * models from the current map are kept seperate
model_t	mod_inline[MAX_MOD_INLINE] EXT_RAM_BSS;

int		registration_sequence;
int		modfilelen;

//===============================================================================


/*
================
Mod_Modellist_f
================
*/
void Mod_Modellist_f (void)
{
	int		i;
	model_t	*mod;
	int		total;

	total = 0;
	ri.Con_Printf (PRINT_ALL,"Loaded models:\n");
	for (i=0, mod=mod_known ; i < mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			continue;
		ri.Con_Printf (PRINT_ALL, "%8i : %s\n",mod->extradatasize, mod->name);
		total += mod->extradatasize;
	}
	ri.Con_Printf (PRINT_ALL, "Total resident: %i\n", total);
}

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
	memset (mod_novis, 0xff, sizeof(mod_novis));
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
static model_t *Mod_ForName_ (char *name, qboolean crash)
{
	model_t	*mod;
	unsigned *buf;
	int		i;
	
	if (!name[0])
		ri.Sys_Error (ERR_DROP,"Mod_ForName: NULL name");

	//
	// inline models are grabbed only from worldmodel
	//
	if (name[0] == '*')
	{
		i = atoi(name+1);
		if (i < 1 || !r_worldmodel || i >= r_worldmodel->numsubmodels)
			ri.Sys_Error (ERR_DROP, "bad inline model number");
		return &mod_inline[i];
	}

	//
	// search the currently loaded models
	//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
		if (!strcmp (mod->name, name) )
			return mod;
			
	//
	// find a free model slot spot
	//
	for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			break;	// free spot
	}
	if (i == mod_numknown)
	{
		if (mod_numknown == MAX_MOD_KNOWN)
			ri.Sys_Error (ERR_DROP, "mod_numknown == MAX_MOD_KNOWN");
		mod_numknown++;
	}
	strcpy (mod->name, name);
	
	//
	// load the file: a BSP is read one lump at a time, the other models
	// are loaded whole
	//
	loadmodel = mod;
	buf = NULL;

	// ESP32: a model in the flash asset partition is used in place
	{
		extern const void *QG_FindAsset (const char *name, int *size);
		int		asize;
		dmdl_t	*ph = (dmdl_t *)QG_FindAsset (mod->name, &asize);

		if (ph && ph->ident == IDALIASHEADER && ph->version == ALIAS_VERSION)
		{
			int		i;

			mod->type = mod_alias;
			mod->extradata = ph;
			mod->extradatasize = 0;
			for (i=0 ; i<ph->num_skins ; i++)
				mod->skins[i] = R_FindImage ((char *)ph + ph->ofs_skins + i*MAX_SKINNAME, it_skin);
			return mod;
		}
	}

	{
		FILE	*h;
		unsigned ident = 0;

		if (mod_file)
		{
			fclose (mod_file);	// left open by a load aborted by an error
			mod_file = NULL;
		}
		modfilelen = FS_FOpenFile (mod->name, &h);
		if (!h)
		{
			if (crash)
				ri.Sys_Error (ERR_DROP,"Mod_NumForName: %s not found", mod->name);
			memset (mod->name, 0, sizeof(mod->name));
			return NULL;
		}
		mod_filebase = ftell (h);
		FS_Read (&ident, 4, h);
		if (LittleLong (ident) == IDBSPHEADER)
		{
			mod_file = h;
			Mod_LoadBrushModel (mod, NULL);
			fclose (h);
			mod_file = NULL;
		}
		else if (LittleLong (ident) == IDALIASHEADER)
		{
			// ESP32: a model not in the flash assets (too big) is read
			// straight into its hunk, without a file buffer
			dmdl_t	hdr;

			mod_file = h;
			FS_ReadAt (h, mod_filebase, 0, &hdr, sizeof(hdr));
			loadmodel->extradata = Hunk_Begin (Mod_AliasSize (&hdr));
			Mod_LoadAliasModel (mod, NULL);
			fclose (h);
			mod_file = NULL;
		}
		else
		{
			fclose (h);
			modfilelen = ri.FS_LoadFile (mod->name, (void **)&buf);
			if (!buf)
				ri.Sys_Error (ERR_DROP,"Mod_NumForName: %s not found", mod->name);

			switch (LittleLong(*(unsigned *)buf))
			{
			case IDALIASHEADER:
				loadmodel->extradata = Hunk_Begin (Mod_AliasSize ((dmdl_t *)buf));
				Mod_LoadAliasModel (mod, buf);
				break;

			case IDSPRITEHEADER:
				loadmodel->extradata = Hunk_Begin (modfilelen);
				Mod_LoadSpriteModel (mod, buf);
				break;

			default:
				ri.Sys_Error (ERR_DROP,"Mod_NumForName: unknown fileid for %s", mod->name);
				break;
			}
		}
	}

	loadmodel->extradatasize = Hunk_End ();

	extern void ri_esp_log_model (const char *name, int filelen, int datasize);
	ri_esp_log_model (mod->name, modfilelen, loadmodel->extradatasize);

	if (buf)
		ri.FS_FreeFile (buf);

	return mod;
}


/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
	mnode_t		*node;
	float		d;
	mplane_t	*plane;
	
	if (!model || !model->nodes)
		ri.Sys_Error (ERR_DROP, "Mod_PointInLeaf: bad model");

	node = model->nodes;
	while (1)
	{
		if (node->contents != -1)
			return (mleaf_t *)node;
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}
	
	return NULL;	// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
byte *Mod_DecompressVis (byte *in, model_t *model)
{
	static byte	decompressed[MAX_MAP_LEAFS/8] EXT_RAM_BSS;
	int		c;
	byte	*out;
	int		row;

	row = (model->vis->numclusters+7)>>3;	
	out = decompressed;

#if 0
	memcpy (out, in, row);
#else
	if (!in)
	{	// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}
		return decompressed;		
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}
	
		c = in[1];
		in += 2;
		while (c)
		{
			*out++ = 0;
			c--;
		}
	} while (out - decompressed < row);
#endif
	
	return decompressed;
}

/*
==============
Mod_ClusterPVS
==============
*/
byte *Mod_ClusterPVS (int cluster, model_t *model)
{
	if (cluster == -1 || !model->vis)
		return mod_novis;
	return Mod_DecompressVis ( (byte *)model->vis + model->vis->bitofs[cluster][DVIS_PVS],
		model);
}

/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

byte	*mod_base;

// Reads a lump of the BSP being loaded into a transient buffer, pointed to
// by mod_base + l->fileofs as when the whole file was loaded
// ESP32: when the arena has no block for it, the lump is read into the
// surface cache, unused while a map is loaded
static qboolean	mod_lump_in_surfcache;

static void *Mod_LumpBegin (lump_t *l)
{
	byte	*buf = Q_Arena_AllocTop (l->filelen + 1);
	int		size;

	mod_lump_in_surfcache = false;
	if (!buf)
	{
		buf = D_BorrowSurfaceCache (&size);
		if (buf && l->filelen + 1 > size)
		{
			D_ReturnSurfaceCache ();
			buf = NULL;
		}
		mod_lump_in_surfcache = buf != NULL;
	}
	if (!buf)
		ri.Sys_Error (ERR_DROP, "Mod_LoadBrushModel: no memory for a %d byte lump", l->filelen);
	FS_ReadAt (mod_file, mod_filebase, l->fileofs, buf, l->filelen);
	mod_base = buf - l->fileofs;
	return buf;
}

static void Mod_LumpEnd (void *buf)
{
	if (mod_lump_in_surfcache)
		D_ReturnSurfaceCache ();
	else
		Q_Arena_Free (buf);
	mod_lump_in_surfcache = false;
	mod_base = NULL;
}


/*
=================
Mod_LoadLighting

Converts the 24 bit lighting down to 8 bit
by taking the brightest component
=================
*/
void Mod_LoadLighting (lump_t *l)
{
	int		i, size, done, chunk;
	byte	*in, *out;
	byte	buf[3 * 1024];

	if (!l->filelen)
	{
		loadmodel->lightdata = NULL;
		return;
	}
	size = l->filelen/3;
	loadmodel->lightdata = out = Hunk_Alloc (size);
	// read and convert in small chunks: this lump can be over 1 MB
	for (done = 0 ; done < size ; done += chunk)
	{
		chunk = size - done;
		if (chunk > sizeof(buf) / 3)
			chunk = sizeof(buf) / 3;
		FS_ReadAt (mod_file, mod_filebase, l->fileofs + done * 3, buf, chunk * 3);
		for (i=0, in=buf ; i<chunk ; i++, in+=3)
		{
			if (in[0] > in[1] && in[0] > in[2])
				*out++ = in[0];
			else if (in[1] > in[0] && in[1] > in[2])
				*out++ = in[1];
			else
				*out++ = in[2];
		}
	}
}


// ESP32: the r_leaftovis / r_vistoleaf tables (256 kB) were never read
int		r_numvisleafs;

void	R_NumberLeafs (mnode_t *node)
{
	mleaf_t	*leaf;
	int		leafnum;

	if (node->contents != -1)
	{
		leaf = (mleaf_t *)node;
		leafnum = leaf - loadmodel->leafs;
		if (leaf->contents & CONTENTS_SOLID)
			return;
		r_numvisleafs++;
		return;
	}

	R_NumberLeafs (node->children[0]);
	R_NumberLeafs (node->children[1]);
}


/*
=================
Mod_LoadVisibility
=================
*/
void Mod_LoadVisibility (lump_t *l)
{
	int		i;

	if (!l->filelen)
	{
		loadmodel->vis = NULL;
		return;
	}
	if (shared_vis)
	{
		loadmodel->vis = shared_vis;
		return;
	}
	loadmodel->vis = Hunk_Alloc ( l->filelen);	
	FS_ReadAt (mod_file, mod_filebase, l->fileofs, loadmodel->vis, l->filelen);

	loadmodel->vis->numclusters = LittleLong (loadmodel->vis->numclusters);
	for (i=0 ; i<loadmodel->vis->numclusters ; i++)
	{
		loadmodel->vis->bitofs[i][0] = LittleLong (loadmodel->vis->bitofs[i][0]);
		loadmodel->vis->bitofs[i][1] = LittleLong (loadmodel->vis->bitofs[i][1]);
	}
}


/*
=================
Mod_LoadVertexes
=================
*/
void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t	*in;
	mvertex_t	*out;
	int			i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count+8)*sizeof(*out));		// extra for skybox

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->position[0] = LittleFloat (in->point[0]);
		out->position[1] = LittleFloat (in->point[1]);
		out->position[2] = LittleFloat (in->point[2]);
	}
}

/*
=================
Mod_LoadSubmodels
=================
*/
void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t	*in;
	dmodel_t	*out;
	int			i, j, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));	

	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;
	if (count > MAX_MOD_INLINE)
		ri.Sys_Error (ERR_DROP, "%s has too many submodels (%d)", loadmodel->name, count);

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
			out->origin[j] = LittleFloat (in->origin[j]);
		}
		out->headnode = LittleLong (in->headnode);
		out->firstface = LittleLong (in->firstface);
		out->numfaces = LittleLong (in->numfaces);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
void Mod_LoadEdges (lump_t *l)
{
	dedge_t *in;
	medge_t *out;
	int 	i, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count + 13) * sizeof(*out));	// extra for skybox

	loadmodel->edges = out;
	loadmodel->numedges = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->v[0] = (unsigned short)LittleShort(in->v[0]);
		out->v[1] = (unsigned short)LittleShort(in->v[1]);
	}
}

/*
=================
Mod_LoadTexinfo
=================
*/
void Mod_LoadTexinfo (lump_t *l)
{
	texinfo_t *in;
	mtexinfo_t *out, *step;
	int 	i, j, count;
	float	len1, len2;
	char	name[MAX_QPATH];
	int		next;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count+6)*sizeof(*out));	// extra for skybox
	// the pooled hunk reuses blocks, so stale pointers from a previous map
	// would survive into out->next (left untouched when nexttexinfo is 0)
	// and crash the animation-frame walk below
	memset (out, 0, (count+6)*sizeof(*out));

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<8 ; j++)
			out->vecs[0][j] = LittleFloat (in->vecs[0][j]);
		len1 = VectorLength (out->vecs[0]);
		len2 = VectorLength (out->vecs[1]);
		len1 = (len1 + len2)/2;
		if (len1 < 0.32)
			out->mipadjust = 4;
		else if (len1 < 0.49)
			out->mipadjust = 3;
		else if (len1 < 0.99)
			out->mipadjust = 2;
		else
			out->mipadjust = 1;
#if 0
		if (len1 + len2 < 0.001)
			out->mipadjust = 1;		// don't crash
		else
			out->mipadjust = 1 / floor( (len1+len2)/2 + 0.1 );
#endif

		out->flags = LittleLong (in->flags);

		next = LittleLong (in->nexttexinfo);
		if (next > 0)
			out->next = loadmodel->texinfo + next;

		Com_sprintf (name, sizeof(name), "textures/%s.wal", in->texture);
		// ESP32: these surfaces are drawn from mip level 0 (the others from
		// the surface cache, at sw_mipcap and up)
		{
			extern int r_wal_minmip;
			extern cvar_t *sw_mipcap;
			r_wal_minmip = (out->flags & (SURF_WARP|SURF_FLOWING|SURF_SKY|SURF_TRANS33|SURF_TRANS66))
				|| sw_mipcap->value < 1 ? 0 : 1;
		}
		out->image = R_FindImage (name, it_wall);
		if (!out->image)
		{
			out->image = r_notexture_mip; // texture not found
			out->flags = 0;
		}
	}

	// count animation frames
	for (i=0 ; i<count ; i++)
	{
		out = &loadmodel->texinfo[i];
		out->numframes = 1;
		for (step = out->next ; step && step != out ; step=step->next)
			out->numframes++;
	}
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
void CalcSurfaceExtents (msurface_t *s)
{
	float	mins[2], maxs[2], val;
	int		i,j, e;
	mvertex_t	*v;
	mtexinfo_t	*tex;
	int		bmins[2], bmaxs[2];

	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;

	tex = s->texinfo;
	
	for (i=0 ; i<s->numedges ; i++)
	{
		e = loadmodel->surfedges[s->firstedge+i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];
		
		for (j=0 ; j<2 ; j++)
		{
			val = v->position[0] * tex->vecs[j][0] + 
				v->position[1] * tex->vecs[j][1] +
				v->position[2] * tex->vecs[j][2] +
				tex->vecs[j][3];
			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}

	for (i=0 ; i<2 ; i++)
	{	
		bmins[i] = floor(mins[i]/16);
		bmaxs[i] = ceil(maxs[i]/16);

		s->texturemins[i] = bmins[i] * 16;
		s->extents[i] = (bmaxs[i] - bmins[i]) * 16;
		if (s->extents[i] < 16)
			s->extents[i] = 16;	// take at least one cache block
		if ( !(tex->flags & (SURF_WARP|SURF_SKY)) && s->extents[i] > 256)
			ri.Sys_Error (ERR_DROP,"Bad surface extents");
	}
}


/*
=================
Mod_LoadFaces
=================
*/
void Mod_LoadFaces (lump_t *l)
{
	dface_t		*in;
	msurface_t 	*out;
	int			i, count, surfnum;
	int			planenum, side;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count+6)*sizeof(*out));	// extra for skybox
	// pooled hunk returns reused blocks; clear the per-frame render state
	// (cachespots, dlightframe, cachedlight) that Mod_LoadBrushModel does not
	// write, or stale pointers to the previous map's surface cache crash
	// D_CacheSurface
	memset (out, 0, (count+6)*sizeof(*out));

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	for ( surfnum=0 ; surfnum<count ; surfnum++, in++, out++)
	{
		out->firstedge = LittleLong(in->firstedge);
		out->numedges = LittleShort(in->numedges);		
		if (out->numedges < 3)
			ri.Sys_Error (ERR_DROP,"Surface with %s edges", out->numedges);
		out->flags = 0;

		planenum = LittleShort(in->planenum);
		side = LittleShort(in->side);
		if (side)
			out->flags |= SURF_PLANEBACK;			

		out->plane = loadmodel->planes + planenum;

		out->texinfo = loadmodel->texinfo + LittleShort (in->texinfo);

		CalcSurfaceExtents (out);
				
	// lighting info is converted from 24 bit on disk to 8 bit

		for (i=0 ; i<MAXLIGHTMAPS ; i++)
			out->styles[i] = in->styles[i];
		i = LittleLong(in->lightofs);
		if (i == -1)
			out->samples = NULL;
		else
			out->samples = loadmodel->lightdata + i/3;
		
	// set the drawing flags flag
		
		if (!out->texinfo->image)
			continue;
		if (out->texinfo->flags & SURF_SKY)
		{
			out->flags |= SURF_DRAWSKY;
			continue;
		}
		
		if (out->texinfo->flags & SURF_WARP)
		{
			out->flags |= SURF_DRAWTURB;
			for (i=0 ; i<2 ; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}
			continue;
		}
//==============
//PGM
		// this marks flowing surfaces as turbulent, but with the new
		// SURF_FLOW flag.
		if (out->texinfo->flags & SURF_FLOWING)
		{
			out->flags |= SURF_DRAWTURB | SURF_FLOW;
			for (i=0 ; i<2 ; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}
			continue;
		}
//PGM
//==============
	}
}


/*
=================
Mod_SetParent
=================
*/
void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents != -1)
		return;
	Mod_SetParent (node->children[0], node);
	Mod_SetParent (node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
void Mod_LoadNodes (lump_t *l)
{
	int			i, j, count, p;
	dnode_t		*in;
	mnode_t 	*out;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));	

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}
	
		p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = LittleShort (in->firstface);
		out->numsurfaces = LittleShort (in->numfaces);
		out->contents = CONTENTS_NODE;	// differentiate from leafs
		
		for (j=0 ; j<2 ; j++)
		{
			p = LittleLong (in->children[j]);
			if (p >= 0)
				out->children[j] = loadmodel->nodes + p;
			else
				out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
		}
	}
	
	Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
void Mod_LoadLeafs (lump_t *l)
{
	dleaf_t 	*in;
	mleaf_t 	*out;
	int			i, j, count;

	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		for (j=0 ; j<3 ; j++)
		{
			out->minmaxs[j] = LittleShort (in->mins[j]);
			out->minmaxs[3+j] = LittleShort (in->maxs[j]);
		}

		out->contents = LittleLong(in->contents);
		out->cluster = LittleShort(in->cluster);
		out->area = LittleShort(in->area);

		out->firstmarksurface = loadmodel->marksurfaces +
			LittleShort(in->firstleafface);
		out->nummarksurfaces = LittleShort(in->numleaffaces);
	}	
}


/*
=================
Mod_LoadMarksurfaces
=================
*/
void Mod_LoadMarksurfaces (lump_t *l)
{	
	int		i, j, count;
	short		*in;
	msurface_t **out;
	
	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( count*sizeof(*out));	

	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for ( i=0 ; i<count ; i++)
	{
		j = LittleShort(in[i]);
		if (j >= loadmodel->numsurfaces)
			ri.Sys_Error (ERR_DROP,"Mod_ParseMarksurfaces: bad surface number");
		out[i] = loadmodel->surfaces + j;
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
void Mod_LoadSurfedges (lump_t *l)
{	
	int		i, count;
	int		*in, *out;
	
	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count+24)*sizeof(*out));	// extra for skybox

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for ( i=0 ; i<count ; i++)
		out[i] = LittleLong (in[i]);
}

/*
=================
Mod_LoadPlanes
=================
*/
void Mod_LoadPlanes (lump_t *l)
{
	int			i, j;
	mplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;
	
	if (shared_planes)
	{
		loadmodel->planes = shared_planes;
		loadmodel->numplanes = shared_numplanes;
		return;
	}
	in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		ri.Sys_Error (ERR_DROP,"MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
	count = l->filelen / sizeof(*in);
	out = Hunk_Alloc ( (count+6)*sizeof(*out));		// extra for skybox
	
	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = LittleLong (in->type);
		out->signbits = bits;
	}
}


/*
=================
Mod_LoadBrushModel
=================
*/
void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
	int			i;
	dheader_t	*header, dheader;
	dmodel_t 	*bm;
	
	loadmodel->type = mod_brush;
	if (loadmodel != mod_known)
		ri.Sys_Error (ERR_DROP, "Loaded a brush model after the world");
	
	header = &dheader;
	FS_ReadAt (mod_file, mod_filebase, 0, header, sizeof(*header));

	i = LittleLong (header->version);
	if (i != BSPVERSION)
		ri.Sys_Error (ERR_DROP,"Mod_LoadBrushModel: %s has wrong version number (%i should be %i)", mod->name, i, BSPVERSION);

// swap all the lumps
	for (i=0 ; i<sizeof(dheader_t)/4 ; i++)
		((int *)header)[i] = LittleLong ( ((int *)header)[i]);

	shared_planes = (mplane_t *)CM_SharedPlanes (mod->name, &shared_numplanes);
	shared_vis = CM_SharedVis (mod->name);

// the hunk is sized for the data (Hunk_Alloc rounds to 16 bytes)
#define LUMPCOUNT(lump, type)	(header->lumps[lump].filelen / sizeof(type))
#define HSIZE(bytes)			(((bytes) + 15) & ~15)
	loadmodel->extradata = Hunk_Begin (
		HSIZE((LUMPCOUNT(LUMP_VERTEXES, dvertex_t) + 8) * sizeof(mvertex_t))
		+ HSIZE((LUMPCOUNT(LUMP_EDGES, dedge_t) + 13) * sizeof(medge_t))
		+ HSIZE((LUMPCOUNT(LUMP_SURFEDGES, int) + 24) * sizeof(int))
		+ HSIZE(header->lumps[LUMP_LIGHTING].filelen / 3)
		+ (shared_planes ? 0 : HSIZE((LUMPCOUNT(LUMP_PLANES, dplane_t) + 6) * sizeof(mplane_t)))
		+ HSIZE((LUMPCOUNT(LUMP_TEXINFO, texinfo_t) + 6) * sizeof(mtexinfo_t))
		+ HSIZE((LUMPCOUNT(LUMP_FACES, dface_t) + 6) * sizeof(msurface_t))
		+ HSIZE(LUMPCOUNT(LUMP_LEAFFACES, short) * sizeof(msurface_t *))
		+ (shared_vis ? 0 : HSIZE(header->lumps[LUMP_VISIBILITY].filelen))
		+ HSIZE(LUMPCOUNT(LUMP_LEAFS, dleaf_t) * sizeof(mleaf_t))
		+ HSIZE(LUMPCOUNT(LUMP_NODES, dnode_t) * sizeof(mnode_t))
		+ HSIZE(LUMPCOUNT(LUMP_MODELS, dmodel_t) * sizeof(dmodel_t)));
#undef LUMPCOUNT
#undef HSIZE

// load into heap
	{
		static const int lumporder[] = { LUMP_VERTEXES, LUMP_EDGES, LUMP_SURFEDGES, LUMP_PLANES,
			LUMP_TEXINFO, LUMP_FACES, LUMP_LEAFFACES, LUMP_LEAFS, LUMP_NODES, LUMP_MODELS };
		static void (*const loaders[])(lump_t *) = { Mod_LoadVertexes, Mod_LoadEdges, Mod_LoadSurfedges,
			Mod_LoadPlanes, Mod_LoadTexinfo, Mod_LoadFaces, Mod_LoadMarksurfaces, Mod_LoadLeafs,
			Mod_LoadNodes, Mod_LoadSubmodels };
		for (i=0 ; i<sizeof(lumporder)/sizeof(lumporder[0]) ; i++)
		{
			void *buf;
			if (lumporder[i] == LUMP_PLANES)
				Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);	// needed by the faces
			if (lumporder[i] == LUMP_LEAFS)
				Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
			buf = Mod_LumpBegin (&header->lumps[lumporder[i]]);
			loaders[i] (&header->lumps[lumporder[i]]);
			Mod_LumpEnd (buf);
		}
	}
	r_numvisleafs = 0;
	R_NumberLeafs (loadmodel->nodes);
	
//
// set up the submodels
//
	for (i=0 ; i<mod->numsubmodels ; i++)
	{
		model_t	*starmod;

		bm = &mod->submodels[i];
		starmod = &mod_inline[i];

		*starmod = *loadmodel;
		
		starmod->firstmodelsurface = bm->firstface;
		starmod->nummodelsurfaces = bm->numfaces;
		starmod->firstnode = bm->headnode;
		if (starmod->firstnode >= loadmodel->numnodes)
			ri.Sys_Error (ERR_DROP, "Inline model %i has bad firstnode", i);

		VectorCopy (bm->maxs, starmod->maxs);
		VectorCopy (bm->mins, starmod->mins);
	
		if (i == 0)
			*loadmodel = *starmod;
	}

	R_InitSkyBox ();
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

/*
=================
Mod_LoadAliasModel
=================
*/
// ESP32: the glcmds (OpenGL only) are not loaded when they are at the end of
// the file, as written by the id tools: returns the bytes kept
static int Mod_AliasSize (dmdl_t *pinmodel)
{
	int glcmds = LittleLong (pinmodel->ofs_glcmds);

	if (glcmds >= LittleLong (pinmodel->ofs_skins) + LittleLong (pinmodel->num_skins) * MAX_SKINNAME
		&& glcmds >= LittleLong (pinmodel->ofs_st) + LittleLong (pinmodel->num_st) * (int)sizeof(dstvert_t)
		&& glcmds >= LittleLong (pinmodel->ofs_tris) + LittleLong (pinmodel->num_tris) * (int)sizeof(dtriangle_t)
		&& glcmds >= LittleLong (pinmodel->ofs_frames) + LittleLong (pinmodel->num_frames) * LittleLong (pinmodel->framesize))
		return glcmds;
	return LittleLong (pinmodel->ofs_end);
}

void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
	int					i, j;
	dmdl_t				*pinmodel, *pheader;
	dstvert_t			*pinst, *poutst;
	dtriangle_t			*pintri, *pouttri;
	daliasframe_t		*pinframe, *poutframe;
	int					*pincmd, *poutcmd;
	int					version;
	qboolean			inplace = !buffer;

	if (inplace)
	{
		// ESP32: read from mod_file into the hunk, converted in place
		dmdl_t	hdr;
		int		size;

		FS_ReadAt (mod_file, mod_filebase, 0, &hdr, sizeof(hdr));
		size = Mod_AliasSize (&hdr);
		buffer = Hunk_Alloc (size);
		FS_ReadAt (mod_file, mod_filebase, 0, buffer, size);
	}
	pinmodel = (dmdl_t *)buffer;

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		ri.Sys_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

	if (inplace)
		pheader = pinmodel;		// already in the hunk
	else
		pheader = Hunk_Alloc (Mod_AliasSize (pinmodel));
	
	// byte swap the header fields and sanity check
	for (i=0 ; i<sizeof(dmdl_t)/4 ; i++)
		((int *)pheader)[i] = LittleLong (((int *)buffer)[i]);

	if (pheader->skinheight > MAX_LBM_HEIGHT)
		ri.Sys_Error (ERR_DROP, "model %s has a skin taller than %d", mod->name,
				   MAX_LBM_HEIGHT);

	if (pheader->num_xyz <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no vertices", mod->name);

	if (pheader->num_xyz > MAX_VERTS)
		ri.Sys_Error (ERR_DROP, "model %s has too many vertices", mod->name);

	if (pheader->num_st <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no st vertices", mod->name);

	if (pheader->num_tris <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no triangles", mod->name);

	if (pheader->num_frames <= 0)
		ri.Sys_Error (ERR_DROP, "model %s has no frames", mod->name);

//
// load base s and t vertices (not used in gl version)
//
	pinst = (dstvert_t *) ((byte *)pinmodel + pheader->ofs_st);
	poutst = (dstvert_t *) ((byte *)pheader + pheader->ofs_st);

	for (i=0 ; i<pheader->num_st ; i++)
	{
		poutst[i].s = LittleShort (pinst[i].s);
		poutst[i].t = LittleShort (pinst[i].t);
	}

//
// load triangle lists
//
	pintri = (dtriangle_t *) ((byte *)pinmodel + pheader->ofs_tris);
	pouttri = (dtriangle_t *) ((byte *)pheader + pheader->ofs_tris);

	for (i=0 ; i<pheader->num_tris ; i++)
	{
		for (j=0 ; j<3 ; j++)
		{
			pouttri[i].index_xyz[j] = LittleShort (pintri[i].index_xyz[j]);
			pouttri[i].index_st[j] = LittleShort (pintri[i].index_st[j]);
		}
	}

//
// load the frames
//
	for (i=0 ; i<pheader->num_frames ; i++)
	{
		pinframe = (daliasframe_t *) ((byte *)pinmodel 
			+ pheader->ofs_frames + i * pheader->framesize);
		poutframe = (daliasframe_t *) ((byte *)pheader 
			+ pheader->ofs_frames + i * pheader->framesize);

		if (poutframe != pinframe)
			memcpy(poutframe->name, pinframe->name, sizeof(poutframe->name));
		for (j=0 ; j<3 ; j++)
		{
			poutframe->scale[j] = LittleFloat (pinframe->scale[j]);
			poutframe->translate[j] = LittleFloat (pinframe->translate[j]);
		}
		// verts are all 8 bit, so no swapping needed
		if (poutframe != pinframe)
			memcpy(poutframe->verts, pinframe->verts,
				pheader->num_xyz*sizeof(dtrivertx_t));

	}

	mod->type = mod_alias;

	//
	// load the glcmds
	//
	if (Mod_AliasSize (pinmodel) == pheader->ofs_glcmds)
		pheader->num_glcmds = 0;		// ESP32: not loaded
	pincmd = (int *) ((byte *)pinmodel + pheader->ofs_glcmds);
	poutcmd = (int *) ((byte *)pheader + pheader->ofs_glcmds);
	for (i=0 ; i<pheader->num_glcmds ; i++)
		poutcmd[i] = LittleLong (pincmd[i]);


	// register all skins
	if (pheader != pinmodel)
		memcpy((char *)pheader + pheader->ofs_skins, (char *)pinmodel + pheader->ofs_skins,
			pheader->num_skins*MAX_SKINNAME);
	for (i=0 ; i<pheader->num_skins ; i++)
	{
		mod->skins[i] = R_FindImage ((char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME, it_skin);
	}
}

/*
==============================================================================

SPRITE MODELS

==============================================================================
*/

/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	dsprite_t	*sprin, *sprout;
	int			i;

	sprin = (dsprite_t *)buffer;
	sprout = Hunk_Alloc (modfilelen);

	sprout->ident = LittleLong (sprin->ident);
	sprout->version = LittleLong (sprin->version);
	sprout->numframes = LittleLong (sprin->numframes);

	if (sprout->version != SPRITE_VERSION)
		ri.Sys_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
				 mod->name, sprout->version, SPRITE_VERSION);

	if (sprout->numframes > MAX_MD2SKINS)
		ri.Sys_Error (ERR_DROP, "%s has too many frames (%i > %i)",
				 mod->name, sprout->numframes, MAX_MD2SKINS);

	// byte swap everything
	for (i=0 ; i<sprout->numframes ; i++)
	{
		sprout->frames[i].width = LittleLong (sprin->frames[i].width);
		sprout->frames[i].height = LittleLong (sprin->frames[i].height);
		sprout->frames[i].origin_x = LittleLong (sprin->frames[i].origin_x);
		sprout->frames[i].origin_y = LittleLong (sprin->frames[i].origin_y);
		memcpy (sprout->frames[i].name, sprin->frames[i].name, MAX_SKINNAME);
		mod->skins[i] = R_FindImage (sprout->frames[i].name, it_sprite);
	}

	mod->type = mod_sprite;
}

//=============================================================================

/*
@@@@@@@@@@@@@@@@@@@@@
R_BeginRegistration

Specifies the model that will be used as the world
@@@@@@@@@@@@@@@@@@@@@
*/
void R_BeginRegistration (char *model)
{
	extern qboolean r_registering;
	r_registering = true;

	char	fullname[MAX_QPATH];
	cvar_t	*flushmap;

	registration_sequence++;
	r_oldviewcluster = -1;		// force markleafs
	Com_sprintf (fullname, sizeof(fullname), "maps/%s.bsp", model);

	D_FlushCaches ();
	// free the previous map's images before registering the new ones, so old
	// and new media never coexist for the whole registration pass (the double
	// load peak that starved PSRAM on map changes). Reused textures reload via
	// R_FindImage when their pixels come back NULL.
	R_FreeUnusedImages ();
	// explicitly free the old map if different
	// this guarantees that mod_known[0] is the world map
	flushmap = ri.Cvar_Get ("flushmap", "0", 0);
	if ( strcmp(mod_known[0].name, fullname) || flushmap->value)
		Mod_Free (&mod_known[0]);
	r_worldmodel = R_RegisterModel (fullname);
	R_NewMap ();
}


/*
@@@@@@@@@@@@@@@@@@@@@
R_RegisterModel

@@@@@@@@@@@@@@@@@@@@@
*/
struct model_s *R_RegisterModel (char *name)
{
	model_t	*mod;
	int		i;
	dsprite_t	*sprout;
	dmdl_t		*pheader;

	mod = Mod_ForName (name, false);
	if (mod)
	{
		mod->registration_sequence = registration_sequence;

		// register any images used by the models
		if (mod->type == mod_sprite)
		{
			sprout = (dsprite_t *)mod->extradata;
			for (i=0 ; i<sprout->numframes ; i++)
				mod->skins[i] = R_FindImage (sprout->frames[i].name, it_sprite);
		}
		else if (mod->type == mod_alias)
		{
			pheader = (dmdl_t *)mod->extradata;
			for (i=0 ; i<pheader->num_skins ; i++)
				mod->skins[i] = R_FindImage ((char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME, it_skin);
//PGM
			mod->numframes = pheader->num_frames;
//PGM
		}
		else if (mod->type == mod_brush)
		{
			for (i=0 ; i<mod->numtexinfo ; i++)
				mod->texinfo[i].image->registration_sequence = registration_sequence;
		}
	}
	return mod;
}

/*
@@@@@@@@@@@@@@@@@@@@@
R_EndRegistration

@@@@@@@@@@@@@@@@@@@@@
*/
void R_EndRegistration (void)
{
	extern qboolean r_registering;
	r_registering = false;

	int		i;
	model_t	*mod;

	for (i=0, mod=mod_known ; i<mod_numknown ; i++, mod++)
	{
		if (!mod->name[0])
			continue;
		if (mod->registration_sequence != registration_sequence)
		{	// don't need this model
			Hunk_Free (mod->extradata);
			memset (mod, 0, sizeof(*mod));
		}
		else
		{	// make sure it is paged in
			Com_PageInMemory (mod->extradata, mod->extradatasize);
		}
	}

	R_FreeUnusedImages ();
}


//=============================================================================

/*
================
Mod_Free
================
*/
void Mod_Free (model_t *mod)
{
	Hunk_Free (mod->extradata);
	memset (mod, 0, sizeof(*mod));
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll (void)
{
	int		i;

	// ESP32: the surface cache points into the surfaces of the models:
	// emptied first, else its next flush writes into freed memory
	D_FlushCaches ();

	// ESP32: also the models whose load was aborted by an error (hunk
	// allocated, extradatasize not set yet), whose hunk was otherwise lost
	for (i=0 ; i<mod_numknown ; i++)
	{
		if (mod_known[i].extradatasize || mod_known[i].extradata)
			Mod_Free (&mod_known[i]);
	}
}

// ESP32: timed for the "perf" recorder
model_t *Mod_ForName (char *name, qboolean crash)
{
	model_t	*mod;
	int		opens = perf_active ? Perf_EventCount (PERF_EV_OPENS) : 0;

	Perf_Push (PERF_ASSET);
	mod = Mod_ForName_ (name, crash);
	Perf_Pop ();
	if (perf_active && Perf_EventCount (PERF_EV_OPENS) != opens)
		Perf_Count (PERF_EV_IMAGES, 1);
	return mod;
}
