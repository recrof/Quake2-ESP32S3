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
// snd_mem.c: sound caching

#include "client.h"
#include "../qcommon/q_perf.h"
#include "snd_loc.h"

int			cache_full_cycle;

unsigned	snd_lru_clock;
extern int	snd_cache_bytes;
extern cvar_t	*s_cachesize;

static qboolean S_SfxPlaying (sfx_t *sfx)
{
	int		i;

	for (i=0 ; i<MAX_CHANNELS ; i++)
		if (channels[i].sfx == sfx)
			return true;
	return false;
}

// ESP32: called when the engine memory is full: frees the sounds that are not playing
void S_PurgeCache (void)
{
	S_MakeRoom (0x3fffffff);
}

#define SND_ARENA_RESERVE	(192 * 1024)	// arena memory the sound cache leaves free (when
										// an allocation fails, the sounds are freed anyway)
int Q_Arena_FreeSize (void);

// Frees cached sounds (least recently used first, not playing) until size
// more bytes fit in the budget: s_cachesize, or less when the engine memory
// is low (the sounds are reloaded from the SD card when played again)
int S_CacheBudget (void)
{
	int		budget = (int)s_cachesize->value * 1024;
	int		avail = snd_cache_bytes + Q_Arena_FreeSize () - SND_ARENA_RESERVE;

	return avail < budget ? avail : budget;
}

void S_MakeRoom (int size)
{
	int		budget = S_CacheBudget ();

	while (snd_cache_bytes + size > budget)
	{
		sfx_t	*sfx, *lru = NULL;
		int		i;

		for (i=0, sfx=known_sfx ; i<num_sfx ; i++, sfx++)
			if (sfx->cache && (!lru || sfx->lastused < lru->lastused) && !S_SfxPlaying (sfx))
				lru = sfx;
		if (!lru)
			return;
		S_FreeSfxCache (lru);
	}
}

byte *S_Alloc (int size);

/*
================
ResampleSfx
================
*/
void ResampleSfx (sfx_t *sfx, int inrate, int inwidth, byte *data)
{
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i;
	int		sample, samplefrac, fracstep;
	sfxcache_t	*sc;
	
	sc = sfx->cache;
	if (!sc)
		return;

	stepscale = (float)inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = sc->length / stepscale;
	sc->length = outcount;
	if (sc->loopstart != -1)
		sc->loopstart = sc->loopstart / stepscale;

	sc->speed = dma.speed;
	if (s_loadas8bit->value)
		sc->width = 1;
	else
		sc->width = inwidth;
	sc->stereo = 0;

// resample / decimate to the current source rate

	if (stepscale == 1 && inwidth == 1 && sc->width == 1)
	{
// fast special case
		for (i=0 ; i<outcount ; i++)
			((signed char *)sc->data)[i]
			= (int)( (unsigned char)(data[i]) - 128);
	}
	else
	{
// general case
		samplefrac = 0;
		fracstep = stepscale*256;
		for (i=0 ; i<outcount ; i++)
		{
			srcsample = samplefrac >> 8;
			samplefrac += fracstep;
			if (inwidth == 2)
				sample = LittleShort ( ((short *)data)[srcsample] );
			else
				sample = (int)( (unsigned char)(data[srcsample]) - 128) << 8;
			if (sc->width == 2)
				((short *)sc->data)[i] = sample;
			else
				((signed char *)sc->data)[i] = sample >> 8;
		}
	}
}

//=============================================================================

/*
==============
S_ReadWavinfo

ESP32: GetWavinfo on a file, reading only the chunk headers (the samples
are read by S_LoadSound, a block at a time)
==============
*/
static int S_ReadLong (FILE *h, int base, int ofs)
{
	byte b[4];
	FS_ReadAt (h, base, ofs, b, 4);
	return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

static int S_FindChunk (FILE *h, int base, int filelen, int pos, char *name, int *chunklen)
{
	char	id[4];

	while (pos + 8 <= filelen)
	{
		FS_ReadAt (h, base, pos, id, 4);
		*chunklen = S_ReadLong (h, base, pos + 4);
		if (*chunklen < 0)
			return -1;
		if (!strncmp (id, name, 4))
			return pos;
		pos += 8 + ((*chunklen + 1) & ~1);
	}
	return -1;
}

static wavinfo_t S_ReadWavinfo (char *name, FILE *h, int base, int filelen)
{
	wavinfo_t	info;
	char		id[4];
	byte		fmt[16];
	int			pos, len, samples;

	memset (&info, 0, sizeof(info));
	FS_ReadAt (h, base, 8, id, 4);
	if (filelen < 12 || strncmp (id, "WAVE", 4))
	{
		Com_Printf ("Missing RIFF/WAVE chunks\n");
		return info;
	}
	pos = S_FindChunk (h, base, filelen, 12, "fmt ", &len);
	if (pos < 0)
	{
		Com_Printf ("Missing fmt chunk\n");
		return info;
	}
	FS_ReadAt (h, base, pos + 8, fmt, 16);
	if ((fmt[0] | (fmt[1] << 8)) != 1)
	{
		Com_Printf ("Microsoft PCM format only\n");
		return info;
	}
	info.channels = fmt[2] | (fmt[3] << 8);
	info.rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
	info.width = (fmt[14] | (fmt[15] << 8)) / 8;

	pos = S_FindChunk (h, base, filelen, 12, "cue ", &len);
	if (pos >= 0)
	{
		info.loopstart = S_ReadLong (h, base, pos + 32);
		// if the next chunk is a LIST chunk, look for a cue length marker
		pos = S_FindChunk (h, base, filelen, pos + 8 + ((len + 1) & ~1), "LIST", &len);
		if (pos >= 0 && pos + 32 <= filelen)
		{
			FS_ReadAt (h, base, pos + 28, id, 4);
			if (!strncmp (id, "mark", 4))
				info.samples = info.loopstart + S_ReadLong (h, base, pos + 24);
		}
	}
	else
		info.loopstart = -1;

	pos = S_FindChunk (h, base, filelen, 12, "data", &len);
	if (pos < 0 || !info.width)
	{
		Com_Printf ("Missing data chunk\n");
		info.channels = 0;
		return info;
	}
	samples = len / info.width;
	if (info.samples)
	{
		if (samples < info.samples)
			Com_Error (ERR_DROP, "Sound %s has a bad loop length", name);
	}
	else
		info.samples = samples;
	info.dataofs = pos + 8;
	return info;
}

/*
==============
S_LoadSound
==============
*/
#define SND_READ_BLOCK	(32 * 1024)

static sfxcache_t *S_LoadSound_ (sfx_t *s)
{
    char	namebuffer[MAX_QPATH];
	wavinfo_t	info;
	int		len, size, base, i, outcount, samplefrac, fracstep, srcsample;
	int		inbase, incount;
	float	stepscale;
	sfxcache_t	*sc;
	char	*name;
	FILE	*h;
	byte	smallbuf[2048], *inbuf;
	int		insize;
	// ESP32: the samples are read in large blocks (32 kB): on an SD card on
	// SPI, every read is a few ms of overhead
	static byte	*readbuf;

	if (!readbuf)
		readbuf = malloc (SND_READ_BLOCK);
	inbuf = readbuf ? readbuf : smallbuf;
	insize = readbuf ? SND_READ_BLOCK : sizeof(smallbuf);

	if (s->name[0] == '*')
		return NULL;

// see if still in memory
	sc = s->cache;
	if (sc)
	{
		s->lastused = ++snd_lru_clock;
		return sc;
	}

// load it in
	if (s->truename)
		name = s->truename;
	else
		name = s->name;

	if (name[0] == '#')
		strcpy(namebuffer, &name[1]);
	else
		Com_sprintf (namebuffer, sizeof(namebuffer), "sound/%s", name);

	// ESP32: the samples are read from the file a block at a time and
	// resampled into the cache (some files are over 400 kB)
	size = FS_FOpenFile (namebuffer, &h);
	if (!h)
	{
		Com_DPrintf ("Couldn't load %s\n", namebuffer);
		return NULL;
	}
	base = ftell (h);

	info = S_ReadWavinfo (s->name, h, base, size);
	if (info.channels != 1)
	{
		if (info.channels)
			Com_Printf ("%s is a stereo sample\n",s->name);
		fclose (h);
		return NULL;
	}

	stepscale = (float)info.rate / dma.speed;	
	outcount = info.samples / stepscale;
	// the samples are converted to 8 bits when s_loadas8bit is set
	len = outcount * (s_loadas8bit->value ? 1 : info.width);

	// ESP32: the sound cache has a budget (s_cachesize kB), the least
	// recently used sounds that are not playing are freed to make room
	S_MakeRoom (len + sizeof(sfxcache_t));
	sc = s->cache = Z_TryMallocTop (len + sizeof(sfxcache_t));
	if (!sc)
	{
		// out of memory: free all the cached sounds that are not playing and
		// try again, else play without this sound
		S_MakeRoom (0x3fffffff);
		sc = s->cache = Z_TryMallocTop (len + sizeof(sfxcache_t));
		if (!sc)
		{
			Com_DPrintf ("S_LoadSound: no memory for %s\n", s->name);
			fclose (h);
			return NULL;
		}
	}
	s->lastused = ++snd_lru_clock;
	snd_cache_bytes += len + sizeof(sfxcache_t);

	sc->length = outcount;
	sc->loopstart = info.loopstart == -1 ? -1 : info.loopstart / stepscale;
	sc->speed = dma.speed;
	sc->width = s_loadas8bit->value ? 1 : info.width;
	sc->stereo = 0;

	// resample / decimate to the current source rate
	samplefrac = 0;
	fracstep = stepscale*256;
	inbase = incount = 0;
	for (i=0 ; i<outcount ; i++)
	{
		int sample;

		srcsample = samplefrac >> 8;
		samplefrac += fracstep;
		if (srcsample >= inbase + incount)
		{
			inbase = srcsample;
			incount = info.samples - inbase;
			if (incount > insize / info.width)
				incount = insize / info.width;
			FS_ReadAt (h, base, info.dataofs + inbase * info.width, inbuf, incount * info.width);
		}
		if (info.width == 2)
			sample = LittleShort (((short *)inbuf)[srcsample - inbase]);
		else
			sample = (int)( (unsigned char)(inbuf[srcsample - inbase]) - 128) << 8;
		if (sc->width == 2)
			((short *)sc->data)[i] = sample;
		else
			((signed char *)sc->data)[i] = sample >> 8;
	}
	fclose (h);

	return sc;
}



/*
===============================================================================

WAV loading

===============================================================================
*/


byte	*data_p;
byte 	*iff_end;
byte 	*last_chunk;
byte 	*iff_data;
int 	iff_chunk_len;


short GetLittleShort(void)
{
	short val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	data_p += 2;
	return val;
}

int GetLittleLong(void)
{
	int val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	val = val + (*(data_p+2)<<16);
	val = val + (*(data_p+3)<<24);
	data_p += 4;
	return val;
}

void FindNextChunk(char *name)
{
	while (1)
	{
		data_p=last_chunk;

		if (data_p >= iff_end)
		{	// didn't find the chunk
			data_p = NULL;
			return;
		}
		
		data_p += 4;
		iff_chunk_len = GetLittleLong();
		if (iff_chunk_len < 0)
		{
			data_p = NULL;
			return;
		}
//		if (iff_chunk_len > 1024*1024)
//			Sys_Error ("FindNextChunk: %i length is past the 1 meg sanity limit", iff_chunk_len);
		data_p -= 8;
		last_chunk = data_p + 8 + ( (iff_chunk_len + 1) & ~1 );
		if (!strncmp(data_p, name, 4))
			return;
	}
}

void FindChunk(char *name)
{
	last_chunk = iff_data;
	FindNextChunk (name);
}


void DumpChunks(void)
{
	char	str[5];
	
	str[4] = 0;
	data_p=iff_data;
	do
	{
		memcpy (str, data_p, 4);
		data_p += 4;
		iff_chunk_len = GetLittleLong();
		Com_Printf ("0x%x : %s (%d)\n", (int)(data_p - 4), str, iff_chunk_len);
		data_p += (iff_chunk_len + 1) & ~1;
	} while (data_p < iff_end);
}

/*
============
GetWavinfo
============
*/
wavinfo_t GetWavinfo (char *name, byte *wav, int wavlength)
{
	wavinfo_t	info;
	int     i;
	int     format;
	int		samples;

	memset (&info, 0, sizeof(info));

	if (!wav)
		return info;
		
	iff_data = wav;
	iff_end = wav + wavlength;

// find "RIFF" chunk
	FindChunk("RIFF");
	if (!(data_p && !strncmp(data_p+8, "WAVE", 4)))
	{
		Com_Printf("Missing RIFF/WAVE chunks\n");
		return info;
	}

// get "fmt " chunk
	iff_data = data_p + 12;
// DumpChunks ();

	FindChunk("fmt ");
	if (!data_p)
	{
		Com_Printf("Missing fmt chunk\n");
		return info;
	}
	data_p += 8;
	format = GetLittleShort();
	if (format != 1)
	{
		Com_Printf("Microsoft PCM format only\n");
		return info;
	}

	info.channels = GetLittleShort();
	info.rate = GetLittleLong();
	data_p += 4+2;
	info.width = GetLittleShort() / 8;

// get cue chunk
	FindChunk("cue ");
	if (data_p)
	{
		data_p += 32;
		info.loopstart = GetLittleLong();
//		Com_Printf("loopstart=%d\n", sfx->loopstart);

	// if the next chunk is a LIST chunk, look for a cue length marker
		FindNextChunk ("LIST");
		if (data_p)
		{
			if (!strncmp (data_p + 28, "mark", 4))
			{	// this is not a proper parse, but it works with cooledit...
				data_p += 24;
				i = GetLittleLong ();	// samples in loop
				info.samples = info.loopstart + i;
//				Com_Printf("looped length: %i\n", i);
			}
		}
	}
	else
		info.loopstart = -1;

// find data chunk
	FindChunk("data");
	if (!data_p)
	{
		Com_Printf("Missing data chunk\n");
		return info;
	}

	data_p += 4;
	samples = GetLittleLong () / info.width;

	if (info.samples)
	{
		if (samples < info.samples)
			Com_Error (ERR_DROP, "Sound %s has a bad loop length", name);
	}
	else
		info.samples = samples;

	info.dataofs = data_p - wav;
	
	return info;
}

// ESP32: timed for the "perf" recorder
sfxcache_t *S_LoadSound (sfx_t *s)
{
	sfxcache_t	*sc;

	if (s->cache)
		return S_LoadSound_ (s);	// cached: no loading
	Perf_Push (PERF_SOUND);
	sc = S_LoadSound_ (s);
	Perf_Pop ();
	if (sc)
		Perf_Count (PERF_EV_SOUNDS, 1);
	return sc;
}
