// q_arena.c -- block allocator over one contiguous arena holding all the
// engine memory: zone (Z_Malloc), model hunks, images, sounds, the surface
// cache and the transient file buffers.
//
// Based on the ESP32-P4 port allocator by Alejandro Villegas Alonso. On the
// ESP32-S3 there is only 8 MB of PSRAM, so there is a single arena (no
// separate hunk pool), small headers, and two allocation ends: long lived
// data is taken from the bottom (first fit), transient buffers (file loads)
// from the top (last fit), which keeps the transients from fragmenting the
// resident data. Blocks can be shrunk in place (Hunk_End).

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "q_arena.h"

#define ARENA_ALIGN	16

typedef struct arena_blk_s
{
	struct arena_blk_s	*prev, *next;	// all blocks, in address order
	int		size;		// total bytes of this block including the header
	unsigned	inuse;		// 0 if free, else the address of the caller (debugging)
} arena_blk_t;

#define ARENA_HDR	((int)((sizeof (arena_blk_t) + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1)))
#define MIN_SPLIT	(ARENA_HDR + 64)

static const char *TAG = "ARENA";

static unsigned char	*arena_base;
static int		arena_total;
static int		arena_used;		// bytes in used blocks, headers included
static int		arena_peak;
static int		arena_blocks;
static arena_blk_t	arena_head;		// sentinel
static int		arena_ready;

static void blk_unlink (arena_blk_t *b)
{
	b->prev->next = b->next;
	b->next->prev = b->prev;
	arena_blocks--;
}

static void blk_link_after (arena_blk_t *at, arena_blk_t *b)
{
	b->next = at->next;
	b->prev = at;
	at->next->prev = b;
	at->next = b;
	arena_blocks++;
}

int Q_Arena_InitAt (void *mem, int size)
{
	uintptr_t raw = (uintptr_t)mem;
	unsigned char *aligned = (unsigned char *)((raw + ARENA_ALIGN - 1) & ~(uintptr_t)(ARENA_ALIGN - 1));

	size -= (int)(aligned - (unsigned char *)mem);
	size &= ~(ARENA_ALIGN - 1);
	arena_base = aligned;
	arena_total = size;
	arena_used = 0;
	arena_peak = 0;
	arena_blocks = 0;
	arena_head.size = 0;
	arena_head.inuse = 1;
	arena_head.prev = &arena_head;
	arena_head.next = &arena_head;

	arena_blk_t *b = (arena_blk_t *)arena_base;
	b->size = arena_total;
	b->inuse = 0;
	blk_link_after (&arena_head, b);
	arena_ready = 1;
	ESP_LOGI (TAG, "%d bytes at %p", arena_total, arena_base);
	return arena_total;
}

// Reserves the arena from PSRAM: the largest free block, minus "leave" bytes
// for the C heap (task stacks, drivers, stdio...).
int Q_Arena_Init (int want, int leave)
{
	int size = (int)heap_caps_get_largest_free_block (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) - leave;
	void *m;

	if (want > 0 && (size <= 0 || size > want))
		size = want;
	m = heap_caps_malloc (size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!m)
	{
		ESP_LOGE (TAG, "cannot reserve %d bytes", size);
		return 0;
	}
	return Q_Arena_InitAt (m, size);
}

static void *take (arena_blk_t *b, int need, int fromtop)
{
	int rem = b->size - need;

	if (rem >= MIN_SPLIT)
	{
		if (fromtop)
		{
			// the free part stays at the bottom, the allocation is the tail
			arena_blk_t *nb = (arena_blk_t *)((unsigned char *)b + rem);
			b->size = rem;
			nb->size = need;
			blk_link_after (b, nb);
			b = nb;
		}
		else
		{
			arena_blk_t *nb = (arena_blk_t *)((unsigned char *)b + need);
			nb->size = rem;
			nb->inuse = 0;
			blk_link_after (b, nb);
			b->size = need;
		}
	}
	b->inuse = 1;
	arena_used += b->size;
	if (arena_used > arena_peak)
		arena_peak = arena_used;
	return (unsigned char *)b + ARENA_HDR;
}

// called when an allocation fails, to free caches (sounds) before retrying
void	(*q_arena_purge) (void);

static int need_size (int size)
{
	return ARENA_HDR + ((size + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1));
}

static void *arena_alloc (int size);
static void *arena_alloc_top (int size);

static void *retry (void *(*alloc)(int), int size, void *caller)
{
	void *p = alloc (size);
	static int purging;

	if (!p && q_arena_purge && !purging)
	{
		purging = 1;
		q_arena_purge ();
		purging = 0;
		p = alloc (size);
	}
	if (!p)
		ESP_LOGE (TAG, "alloc %d failed (free=%d largest=%d)", size, Q_Arena_FreeSize (), Q_Arena_Largest ());
	else
		((arena_blk_t *)((unsigned char *)p - ARENA_HDR))->inuse = (unsigned)(uintptr_t)caller | 1;
	return p;
}

// Prints the used blocks of at least minsize bytes, with the address of the
// code that allocated them (xtensa-esp32s3-elf-addr2line)
void Q_Arena_Dump (int minsize)
{
	arena_blk_t *b;

	for (b = arena_head.next ; b != &arena_head ; b = b->next)
		if (b->size >= minsize)
			printf ("%7d %7d %s %08x\n", (int)((unsigned char *)b - arena_base) / 1024, b->size / 1024,
				b->inuse ? "used" : "free", b->inuse);
}

void *Q_Arena_Alloc (int size)
{
	return retry (arena_alloc, size, __builtin_return_address (0));
}

void *Q_Arena_AllocTop (int size)
{
	return retry (arena_alloc_top, size, __builtin_return_address (0));
}

void *Q_Arena_AllocFrom (int size, int top, void *caller)
{
	return retry (top ? arena_alloc_top : arena_alloc, size, caller);
}

static void *arena_alloc (int size)
{
	arena_blk_t *b, *best = NULL;
	int need = need_size (size);

	if (!arena_ready || size < 0)
		return NULL;
	// first fit from the bottom: the long lived data stays packed at the
	// bottom, and the top in one block for the map data (see AllocTop)
	for (b = arena_head.next ; b != &arena_head ; b = b->next)
	{
		if (!b->inuse && b->size >= need)
		{
			best = b;
			break;
		}
	}
	if (!best)
		return NULL;
	return take (best, need, 0);
}

static void *arena_alloc_top (int size)
{
	arena_blk_t *b;
	int need = need_size (size);

	if (!arena_ready || size < 0)
		return NULL;
	for (b = arena_head.prev ; b != &arena_head ; b = b->prev)
		if (!b->inuse && b->size >= need)
			return take (b, need, 1);
	return NULL;
}

static arena_blk_t *coalesce (arena_blk_t *b)
{
	if (b->prev != &arena_head && !b->prev->inuse)
	{
		b->prev->size += b->size;
		b = b->prev;
		blk_unlink (b->next);
	}
	if (b->next != &arena_head && !b->next->inuse)
	{
		b->size += b->next->size;
		blk_unlink (b->next);
	}
	return b;
}

void Q_Arena_Free (void *p)
{
	arena_blk_t *b;

	if (!p)
		return;
	b = (arena_blk_t *)((unsigned char *)p - ARENA_HDR);
	if (!b->inuse)
	{
		ESP_LOGW (TAG, "double free at %p", p);
		return;
	}
	b->inuse = 0;
	arena_used -= b->size;
	coalesce (b);
}

// Gives back the end of a block, keeping its first newsize bytes.
void Q_Arena_Shrink (void *p, int newsize)
{
	arena_blk_t *b = (arena_blk_t *)((unsigned char *)p - ARENA_HDR);
	int need = need_size (newsize);
	int rem = b->size - need;

	// a small tail is only worth giving back to a free neighbour
	if (rem < ARENA_HDR || (rem < MIN_SPLIT && (b->next == &arena_head || b->next->inuse)))
		return;
	arena_blk_t *nb = (arena_blk_t *)((unsigned char *)b + need);
	nb->size = rem;
	nb->inuse = 0;
	b->size = need;
	arena_used -= rem;
	blk_link_after (b, nb);
	coalesce (nb);
}

int Q_Arena_BlockSize (void *p)
{
	arena_blk_t *b = (arena_blk_t *)((unsigned char *)p - ARENA_HDR);
	return b->size - ARENA_HDR;
}

int Q_Arena_Used (void)
{
	return arena_used;
}

int Q_Arena_Peak (void)
{
	return arena_peak;
}

void Q_Arena_ResetPeak (void)
{
	arena_peak = arena_used;
}

int Q_Arena_Total (void)
{
	return arena_total;
}

int Q_Arena_FreeSize (void)
{
	return arena_total - arena_used;
}

int Q_Arena_Largest (void)
{
	int max = 0;
	arena_blk_t *b;

	for (b = arena_head.next ; b != &arena_head ; b = b->next)
		if (!b->inuse && b->size > max)
			max = b->size;
	return max > ARENA_HDR ? max - ARENA_HDR : 0;
}

int Q_Arena_Count (void)
{
	return arena_blocks;
}

int Q_Arena_Contains (void *p)
{
	unsigned char *c = (unsigned char *)p;
	return arena_ready && c >= arena_base && c < arena_base + arena_total;
}
