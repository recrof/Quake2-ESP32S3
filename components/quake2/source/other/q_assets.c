// q_assets.c -- flash asset image: models and decoded skins stored in their
// in-memory format, used in place by the renderer. The platform maps the image and
// passes it to Q_Assets_Init. The image is built on the board from the PAK files
// (Q_Assets_Build), or by tools/mkassets.py.
//
// Image format (little endian):
//   header:  "Q2AS", version, number of entries, directory offset;
//            version 2 (built on the board): PAK fingerprint, padding to 32 bytes
//   entries: name (56 bytes, lower case, sorted), offset, size
//   data:    16-byte aligned. Models: MD2 without the OpenGL commands. Skins: width,
//            height (uint16), transparent (uint8), 3 padding bytes, then the pixels.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "q_assets.h"

#define ASSET_ALIGN		16
#define MAX_PAKS		10

typedef struct
{
	char	name[56];
	int		offset, size;
} assetentry_t;

typedef struct
{
	char		magic[4];
	int			version, count, diroffset;
	unsigned	key;
	int			pad[3];
} assetheader_t;

static const unsigned char	*asset_base;
static const assetentry_t	*asset_dir;
static int					asset_count;

static int Q_Assets_Valid (const void *base, int size)
{
	const assetheader_t *h = base;

	return base && size >= (int)sizeof(*h) && !memcmp (h->magic, "Q2AS", 4) && (h->version == 1 || h->version == 2)
		&& h->count >= 0 && h->diroffset + h->count * (int)sizeof(assetentry_t) <= size;
}

int Q_Assets_Init (const void *base, int size)
{
	const assetheader_t *h = base;

	if (!Q_Assets_Valid (base, size))
	{
		asset_count = 0;
		return 0;
	}
	asset_base = base;
	asset_count = h->count;
	asset_dir = (const assetentry_t *)(asset_base + h->diroffset);
	printf ("Flash assets: %d files\n", asset_count);
	return asset_count;
}

const void *QG_FindAsset (const char *name, int *size)
{
	int lo = 0, hi = asset_count - 1;

	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		int c = strcasecmp (name, asset_dir[mid].name);
		if (!c)
		{
			if (size)
				*size = asset_dir[mid].size;
			return asset_base + asset_dir[mid].offset;
		}
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}

/*
==============================================================================

Building the image from the PAK files (as tools/mkassets.py, except that identical
files are stored twice: the data is written while it is read)

==============================================================================
*/

// left out: Jorg is only on boss2, which has room for it; the Makron (on boss1 and
// boss2) then fits in flash
static const char *in_psram[] = { "models/monsters/boss3/jorg/tris.md2", NULL };

typedef struct
{
	char	name[56];
	FILE	*pak;
	int		pakofs, paksize;
	int		skin;			// PCX skin, else MD2 model
	int		size;			// in the image
	int		order;			// in the PAK files (stable sort)
	int		keep;
	int		offset;
} assetsrc_t;

typedef struct
{
	FILE	*f[MAX_PAKS];
	int		count;
} paks_t;

static int RdInt (const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static int ReadAt (FILE *f, int ofs, void *buf, int size)
{
	return fseek (f, ofs, SEEK_SET) == 0 && (int)fread (buf, 1, size, f) == size;
}

static unsigned Fnv (unsigned h, const void *data, int size)
{
	const unsigned char *p = data;

	while (size--)
		h = (h ^ *p++) * 0x01000193u;
	return h;
}

// opens the PAK files, highest number first (search order: pak2 overrides pak0)
static void OpenPaks (const char *gamedir, paks_t *paks)
{
	char path[256];
	int i;

	paks->count = 0;
	for (i = MAX_PAKS - 1; i >= 0; i--)
	{
		snprintf (path, sizeof(path), "%s/pak%d.pak", gamedir, i);
		FILE *f = fopen (path, "rb");
		if (f)
		{
			setvbuf (f, NULL, _IOFBF, 16384);	// the default (128 bytes) is very slow on the SD card
			paks->f[paks->count++] = f;
		}
	}
}

static void ClosePaks (paks_t *paks)
{
	while (paks->count)
		fclose (paks->f[--paks->count]);
}

// PAK directory, NULL on error
static unsigned char *ReadPakDir (FILE *f, int *count, unsigned *key)
{
	unsigned char head[12];
	unsigned char *dir;
	int ofs, len;

	if (!ReadAt (f, 0, head, 12) || memcmp (head, "PACK", 4))
		return NULL;
	ofs = RdInt (head + 4);
	len = RdInt (head + 8);
	if (len < 0 || len > 0x1000000 || !(dir = malloc (len ? len : 1)))
		return NULL;
	if (!ReadAt (f, ofs, dir, len))
	{
		free (dir);
		return NULL;
	}
	if (key)
	{
		fseek (f, 0, SEEK_END);
		long size = ftell (f);
		*key = Fnv (Fnv (*key, &size, sizeof(size)), dir, len);
	}
	*count = len / 64;
	return dir;
}

unsigned Q_Assets_SourceKey (const char *gamedir)
{
	paks_t paks;
	unsigned key = 0x811c9dc5u;
	int i, n;

	OpenPaks (gamedir, &paks);
	if (!paks.count)
		return 0;
	for (i = 0; i < paks.count; i++)
	{
		unsigned char *dir = ReadPakDir (paks.f[i], &n, &key);
		free (dir);
		key = Fnv (key, &i, sizeof(i));
	}
	ClosePaks (&paks);
	return key ? key : 1;
}

int Q_Assets_Current (const void *base, int size, unsigned key)
{
	const assetheader_t *h = base;

	return Q_Assets_Valid (base, size) && (h->version == 1 || h->key == key);
}

static int Wanted (const char *name)
{
	int len = strlen (name);

	if (len > 4 && !strcmp (name + len - 4, ".md2"))
		return 1;
	if (len > 4 && !strcmp (name + len - 4, ".pcx") && !strncmp (name, "models/", 7))
		return 2;
	return 0;
}

static int InPsram (const char *name)
{
	int i;

	for (i = 0; in_psram[i]; i++)
		if (!strcmp (name, in_psram[i]))
			return 1;
	return 0;
}

// size of the asset in the image from the header of the file, 0 if it is not stored
static int AssetSize (assetsrc_t *a)
{
	unsigned char h[68];

	if (a->skin)
	{
		if (a->paksize < 128 || !ReadAt (a->pak, a->pakofs, h, 12))
			return 0;
		int xmax = h[8] | h[9] << 8, ymax = h[10] | h[11] << 8;
		if (h[0] != 0x0a || h[1] != 5 || h[2] != 1 || h[3] != 8 || xmax >= 640 || ymax >= 480)
			return 0;
		return 8 + (xmax + 1) * (ymax + 1);
	}
	if (a->paksize < 68 || !ReadAt (a->pak, a->pakofs, h, 68))
		return 0;
	int framesize = RdInt (h + 16), num_skins = RdInt (h + 20), num_st = RdInt (h + 28);
	int num_tris = RdInt (h + 32), num_frames = RdInt (h + 40);
	int ofs_skins = RdInt (h + 44), ofs_st = RdInt (h + 48), ofs_tris = RdInt (h + 52);
	int ofs_frames = RdInt (h + 56), ofs_glcmds = RdInt (h + 60);
	if (ofs_glcmds >= ofs_skins + num_skins * 64 && ofs_glcmds >= ofs_st + num_st * 4
		&& ofs_glcmds >= ofs_tris + num_tris * 12 && ofs_glcmds >= ofs_frames + num_frames * framesize
		&& ofs_glcmds >= 68 && ofs_glcmds <= a->paksize)
		return ofs_glcmds;		// without the OpenGL commands
	return a->paksize;
}

// converts the file (data, paksize bytes) to its stored form (a->size bytes) in out
static void Convert (const assetsrc_t *a, unsigned char *data, unsigned char *out)
{
	if (!a->skin)
	{
		memcpy (out, data, a->size);
		if (a->size != a->paksize)
		{
			memset (out + 36, 0, 4);	// num_glcmds
			out[64] = a->size;			// ofs_end
			out[65] = a->size >> 8;
			out[66] = a->size >> 16;
			out[67] = a->size >> 24;
		}
		return;
	}
	int w = (data[8] | data[9] << 8) + 1, h = (data[10] | data[11] << 8) + 1;
	unsigned char *pix = out + 8;
	int src = 128, x, y, i;
	memset (out, 0, 8);
	out[0] = w;
	out[1] = w >> 8;
	out[2] = h;
	out[3] = h >> 8;
	// as LoadPCX: a run past the end of a line spills into the next one, which then
	// overwrites it
	for (y = 0; y < h; y++)
		for (x = 0; x < w; )
		{
			int b = src < a->paksize ? data[src++] : 0, run = 1;
			if ((b & 0xc0) == 0xc0)
			{
				run = b & 0x3f;
				b = src < a->paksize ? data[src++] : 0;
			}
			for (i = 0; i < run; i++, x++)
				if (y * w + x < w * h)
					pix[y * w + x] = b;
		}
	for (i = 0; i < w * h; i++)
		if (pix[i] == 255)
		{
			out[4] = 1;
			break;
		}
}

static int CompareSize (const void *pa, const void *pb)
{
	const assetsrc_t *a = pa, *b = pb;
	int ia = InPsram (a->name), ib = InPsram (b->name);

	if (ia != ib)
		return ia - ib;
	if (a->size != b->size)
		return a->size < b->size ? -1 : 1;
	return a->order - b->order;
}

static int CompareName (const void *pa, const void *pb)
{
	return strcmp (((const assetsrc_t *)pa)->name, ((const assetsrc_t *)pb)->name);
}

int Q_Assets_Build (const char *gamedir, int capacity, const assetwriter_t *w)
{
	paks_t paks;
	assetsrc_t *src = NULL;
	unsigned char *dirbuf = NULL, *data = NULL, *out = NULL;
	int count = 0, max = 0, i, j, kept = 0, result = -1;
	int datamax = 0, outmax = 0;
	unsigned key = Q_Assets_SourceKey (gamedir);

	OpenPaks (gamedir, &paks);
	if (!paks.count)
		return -1;
	// the models and skins, the first PAK that has a file wins
	for (i = 0; i < paks.count; i++)
	{
		int n;
		unsigned char *dir = ReadPakDir (paks.f[i], &n, NULL);
		if (!dir)
			goto done;
		for (j = 0; j < n; j++)
		{
			char name[57];
			int k, l;
			memcpy (name, dir + j * 64, 56);
			name[56] = 0;
			for (l = 0; name[l]; l++)
				name[l] = tolower ((unsigned char)name[l]);
			if (!Wanted (name))
				continue;
			for (k = 0; k < count && strcmp (src[k].name, name); k++)
				;
			if (k < count)
				continue;
			if (count == max)
			{
				max = max ? max * 2 : 256;
				assetsrc_t *s = realloc (src, max * sizeof(*src));
				if (!s)
				{
					free (dir);
					goto done;
				}
				src = s;
			}
			assetsrc_t *a = &src[count];
			memset (a, 0, sizeof(*a));
			strncpy (a->name, name, sizeof(a->name));
			a->pak = paks.f[i];
			a->pakofs = RdInt (dir + j * 64 + 56);
			a->paksize = RdInt (dir + j * 64 + 60);
			a->skin = Wanted (name) == 2;
			a->order = count;
			count++;
		}
		free (dir);
	}
	// sizes in the image (from the headers), then keep the smallest when everything
	// does not fit: the few big ones (boss models) are loaded in PSRAM
	for (i = j = 0; i < count; i++)
	{
		src[i].size = AssetSize (&src[i]);
		if (src[i].size)
			src[j++] = src[i];
	}
	count = j;
	qsort (src, count, sizeof(*src), CompareSize);
	int used = sizeof(assetheader_t);
	for (i = 0; i < count; i++)
	{
		int need = sizeof(assetentry_t) + (src[i].size + ASSET_ALIGN - 1) / ASSET_ALIGN * ASSET_ALIGN;
		if (InPsram (src[i].name) || used + need > capacity)
		{
			printf ("not stored: %s (%d kB)\n", src[i].name, src[i].size / 1024);
			continue;
		}
		src[i].keep = 1;
		used += need;
	}
	for (i = j = 0; i < count; i++)
		if (src[i].keep)
			src[j++] = src[i];
	kept = j;
	qsort (src, kept, sizeof(*src), CompareName);

	// header and directory
	if (!kept)
		goto done;
	int diroffset = sizeof(assetheader_t);
	int datastart, offset = (diroffset + kept * (int)sizeof(assetentry_t) + ASSET_ALIGN - 1) / ASSET_ALIGN * ASSET_ALIGN;
	datastart = offset;
	if (!(dirbuf = calloc (1, offset)))
		goto done;
	assetheader_t *h = (assetheader_t *)dirbuf;
	memset (h->magic, 0xff, 4);
	h->version = 2;
	h->count = kept;
	h->diroffset = diroffset;
	h->key = key;
	for (i = 0; i < kept; i++)
	{
		assetentry_t *e = (assetentry_t *)(dirbuf + diroffset) + i;
		strncpy (e->name, src[i].name, sizeof(e->name));
		e->offset = src[i].offset = offset;
		e->size = src[i].size;
		offset += (src[i].size + ASSET_ALIGN - 1) / ASSET_ALIGN * ASSET_ALIGN;
		if (src[i].paksize > datamax)
			datamax = src[i].paksize;
		if (src[i].size > outmax)
			outmax = src[i].size;
	}
	if (!w->write (w->ctx, 0, dirbuf, datastart))
		goto done;

	// the data
	if (!(data = malloc (datamax)) || !(out = malloc (outmax)))
		goto done;
	for (i = 0; i < kept; i++)
	{
		if (w->progress)
			w->progress (w->ctx, i, kept);
		if (!ReadAt (src[i].pak, src[i].pakofs, data, src[i].paksize))
		{
			printf ("Q_Assets_Build: can't read %s\n", src[i].name);
			goto done;
		}
		Convert (&src[i], data, out);
		if (!w->write (w->ctx, src[i].offset, out, src[i].size))
			goto done;
	}
	if (w->progress)
		w->progress (w->ctx, kept, kept);
	// done: the magic
	if (!w->write (w->ctx, 0, "Q2AS", 4))
		goto done;
	printf ("Flash assets built: %d files, %d kB of %d kB\n", kept, offset / 1024, capacity / 1024);
	result = kept;
done:
	free (out);
	free (data);
	free (dirbuf);
	free (src);
	ClosePaks (&paks);
	return result;
}
