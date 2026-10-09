// q_assets.c -- flash asset image (tools/mkassets.py): models and decoded
// skins stored in their in-memory format, used in place by the renderer.
// The platform maps the image and passes it to Q_Assets_Init.

#include <string.h>
#include <strings.h>
#include <stdio.h>

typedef struct
{
	char	name[56];
	int		offset, size;
} assetentry_t;

static const unsigned char	*asset_base;
static const assetentry_t	*asset_dir;
static int					asset_count;

int Q_Assets_Init (const void *base, int size)
{
	const int *h = base;

	if (!base || size < 16 || memcmp (base, "Q2AS", 4) || h[1] != 1 || h[3] + h[2] * (int)sizeof(assetentry_t) > size)
	{
		asset_count = 0;
		return 0;
	}
	asset_base = base;
	asset_count = h[2];
	asset_dir = (const assetentry_t *)(asset_base + h[3]);
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
