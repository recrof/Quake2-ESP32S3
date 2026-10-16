// q_assets.h -- flash asset image: models and decoded skins in their in-memory format

#ifndef Q_ASSETS_H
#define Q_ASSETS_H

// Uses the image at base (mapped flash), returns the number of assets (0: no valid image)
int Q_Assets_Init (const void *base, int size);
const void *QG_FindAsset (const char *name, int *size);

// Fingerprint of the PAK files of gamedir (0: no PAK file)
unsigned Q_Assets_SourceKey (const char *gamedir);
// The image at base can be used with these PAK files: built from them on the board, or
// made by tools/mkassets.py (which has no fingerprint)
int Q_Assets_Current (const void *base, int size, unsigned key);

typedef struct
{
	// writes at offset (in increasing order, except the header at the end)
	int		(*write) (void *ctx, int offset, const void *data, int size);
	void	(*progress) (void *ctx, int done, int total);
	void	*ctx;
} assetwriter_t;

// Builds the image from the PAK files of gamedir, at most capacity bytes. The first
// bytes (the magic) are written last: an interrupted build is not taken for an image.
// Returns the number of assets, -1 on error.
int Q_Assets_Build (const char *gamedir, int capacity, const assetwriter_t *w);

#endif
