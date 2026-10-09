#ifndef Q_ARENA_H
#define Q_ARENA_H

// Single contiguous arena holding all the engine memory (zone, model hunks,
// images, sounds, surface cache, transient file buffers). See q_arena.c.

int		Q_Arena_Init (int want, int leave);	// reserve from PSRAM (want <= 0: largest block - leave)
int		Q_Arena_InitAt (void *mem, int size);
void	*Q_Arena_Alloc (int size);			// long lived data, from the bottom
void	*Q_Arena_AllocTop (int size);		// transient data, from the top
void	*Q_Arena_AllocFrom (int size, int top, void *caller);	// caller: for Q_Arena_Dump
void	Q_Arena_Free (void *p);
void	Q_Arena_Shrink (void *p, int newsize);
int		Q_Arena_BlockSize (void *p);		// usable bytes of a block
int		Q_Arena_Used (void);				// bytes in used blocks (incl headers)
int		Q_Arena_Peak (void);
void	Q_Arena_ResetPeak (void);
int		Q_Arena_Total (void);
int		Q_Arena_FreeSize (void);
int		Q_Arena_Largest (void);				// largest allocation possible, bytes
int		Q_Arena_Count (void);				// number of blocks (free + used)
int		Q_Arena_Contains (void *p);
extern void	(*q_arena_purge) (void);
void	Q_Arena_Dump (int minsize);	// frees caches when an allocation fails

#endif
