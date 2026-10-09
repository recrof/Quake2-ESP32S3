// q_hunk.c -- model hunks, carved from the engine arena.
//
// Hunk_Begin reserves maxsize bytes (the model loaders compute what they
// need, see r_model.c) and Hunk_End gives the unused end back to the arena.

#include <string.h>

#include "../qcommon/qcommon.h"
#include "q_arena.h"

static unsigned char *membase;
static int hunkmaxsize;
static int cursize;
static int hunk_bytes;		// resident model data

void Hunk_Init (void)
{
	membase = NULL;
	hunk_bytes = 0;
}

void *Hunk_Begin (int maxsize)
{
	// a model load aborted by an error leaves its hunk open: close it (the
	// model keeps it and frees it with the other models)
	if (membase)
		Hunk_End ();
	maxsize = (maxsize + 15) & ~15;
	// big hunks (world models) are taken from the top of the arena, with the
	// collision map: they need large contiguous blocks, which the long lived
	// small allocations at the bottom would fragment
	membase = Q_Arena_AllocFrom (maxsize, maxsize >= 256 * 1024, __builtin_return_address (0));
	if (!membase)
		Com_Error (ERR_DROP, "Hunk_Begin: out of memory (%d bytes needed, %d largest free)",
			maxsize, Q_Arena_Largest ());
	hunkmaxsize = maxsize;
	cursize = 0;
	return membase;
}

void *Hunk_Alloc (int size)
{
	void *p;

	size = (size + 15) & ~15;
	if (cursize + size > hunkmaxsize)
		Sys_Error ("Hunk_Alloc overflow (%d + %d > %d)", cursize, size, hunkmaxsize);
	p = membase + cursize;
	cursize += size;
	memset (p, 0, size);
	return p;
}

int Hunk_End (void)
{
	Q_Arena_Shrink (membase, cursize);
	hunk_bytes += Q_Arena_BlockSize (membase);
	membase = NULL;
	return cursize;
}

void Hunk_Free (void *base)
{
	if (!base || !Q_Arena_Contains (base))
		return;		// ESP32: models in flash are not in the arena
	if (base == membase)
		membase = NULL;		// hunk of a load aborted by an error (not counted)
	else
		hunk_bytes -= Q_Arena_BlockSize (base);
	Q_Arena_Free (base);
}

int Hunk_PoolUsed (void)
{
	return hunk_bytes;
}
