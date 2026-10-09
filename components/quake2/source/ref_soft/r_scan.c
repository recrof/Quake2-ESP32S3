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
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include "r_local.h"
#include "esp_attr.h"

/*
=============
D_WarpScreen

this performs a slight compression of the screen at the same time as
the sine warp, to keep the edges from wrapping
=============
*/
void D_WarpScreen(void) {
  int w, h;
  int u, v, u2, v2;
  byte *dest;
  int *turb;
  int *col;
  byte **row;

  static int cached_width, cached_height;
  static byte *rowptr[1200 + AMP2 * 2];
  static int column[1600 + AMP2 * 2];

  //
  // these are constant over resolutions, and can be saved
  //
  w = r_newrefdef.width;
  h = r_newrefdef.height;
  if (w != cached_width || h != cached_height) {
    cached_width = w;
    cached_height = h;
    for (v = 0; v < h + AMP2 * 2; v++) {
      v2 = (int)((float)v / (h + AMP2 * 2) * r_refdef.vrect.height);
      rowptr[v] = r_warpbuffer + (WARP_WIDTH * v2);
    }

    for (u = 0; u < w + AMP2 * 2; u++) {
      u2 = (int)((float)u / (w + AMP2 * 2) * r_refdef.vrect.width);
      column[u] = u2;
    }
  }

  turb = intsintable + ((int)(r_newrefdef.time * SPEED) & (CYCLE - 1));
  dest = vid.buffer + r_newrefdef.y * vid.rowbytes + r_newrefdef.x;

  for (v = 0; v < h; v++, dest += vid.rowbytes) {
    col = &column[turb[v]];
    row = &rowptr[v];
    for (u = 0; u < w; u += 4) {
      dest[u + 0] = row[turb[u + 0]][col[u + 0]];
      dest[u + 1] = row[turb[u + 1]][col[u + 1]];
      dest[u + 2] = row[turb[u + 2]][col[u + 2]];
      dest[u + 3] = row[turb[u + 3]][col[u + 3]];
    }
  }
}

/*
=============
D_GetSpanState

ESP32: the span drawing state set by D_CalcGradients (globals), for the span
drawers below, which take it as a parameter so that both cores can draw
=============
*/
void D_GetSpanState(spanstate_t *st) {
  st->sdivzstepu = d_sdivzstepu;
  st->tdivzstepu = d_tdivzstepu;
  st->zistepu = d_zistepu;
  st->sdivzstepv = d_sdivzstepv;
  st->tdivzstepv = d_tdivzstepv;
  st->zistepv = d_zistepv;
  st->sdivzorigin = d_sdivzorigin;
  st->tdivzorigin = d_tdivzorigin;
  st->ziorigin = d_ziorigin;
  st->sadjust = sadjust;
  st->tadjust = tadjust;
  st->bbextents = bbextents;
  st->bbextentt = bbextentt;
  st->cacheblock = cacheblock;
  st->cachewidth = cachewidth;
}

// rows: 0 = all the spans, 1 = the even rows, 2 = the odd ones
#define SKIPSPAN(pspan, rows) ((rows) && (((pspan)->v & 1) != (rows) - 1))

/*
=============
Turbulent8

Warping water textures (turb = sintable + time), or scrolling textures
(turb = blanktable: NonTurbulent8)
=============
*/
void Turbulent8(espan_t *pspan, const spanstate_t *st, int *turb, int rows) {
  int count, spancount;
  fixed16_t s, t, snext, tnext, sstep, tstep;
  float sdivz, tdivz, zi, z, du, dv, spancountminus1;
  float sdivz16stepu, tdivz16stepu, zi16stepu;
  unsigned char *pbase, *pdest;
  const fixed16_t sadj = st->sadjust, tadj = st->tadjust;
  const fixed16_t bbs = st->bbextents, bbt = st->bbextentt;

  sstep = 0; // keep compiler happy
  tstep = 0; // ditto

  pbase = (unsigned char *)st->cacheblock;

  sdivz16stepu = st->sdivzstepu * 16;
  tdivz16stepu = st->tdivzstepu * 16;
  zi16stepu = st->zistepu * 16;

  do {
    if (SKIPSPAN(pspan, rows))
      continue;
    pdest = (unsigned char *)((byte *)d_viewbuffer + (r_screenwidth * pspan->v) + pspan->u);

    count = pspan->count;

    // calculate the initial s/z, t/z, 1/z, s, and t and clamp
    du = (float)pspan->u;
    dv = (float)pspan->v;

    sdivz = st->sdivzorigin + dv * st->sdivzstepv + du * st->sdivzstepu;
    tdivz = st->tdivzorigin + dv * st->tdivzstepv + du * st->tdivzstepu;
    zi = st->ziorigin + dv * st->zistepv + du * st->zistepu;
    z = (float)0x10000 * R_Recip (zi); // prescale to 16.16 fixed-point

    s = (int)(sdivz * z) + sadj;
    if (s > bbs)
      s = bbs;
    else if (s < 0)
      s = 0;

    t = (int)(tdivz * z) + tadj;
    if (t > bbt)
      t = bbt;
    else if (t < 0)
      t = 0;

    do {
      // calculate s and t at the far end of the span
      if (count >= 16)
        spancount = 16;
      else
        spancount = count;

      count -= spancount;

      if (count) {
        // calculate s/z, t/z, zi->fixed s and t at far end of span,
        // calculate s and t steps across span by shifting
        sdivz += sdivz16stepu;
        tdivz += tdivz16stepu;
        zi += zi16stepu;
        z = (float)0x10000 * R_Recip (zi); // prescale to 16.16 fixed-point

        snext = (int)(sdivz * z) + sadj;
        if (snext > bbs)
          snext = bbs;
        else if (snext < 16)
          snext = 16; // prevent round-off error on <0 steps from
                      //  from causing overstepping & running off the
                      //  edge of the texture

        tnext = (int)(tdivz * z) + tadj;
        if (tnext > bbt)
          tnext = bbt;
        else if (tnext < 16)
          tnext = 16; // guard against round-off error on <0 steps

        sstep = (snext - s) >> 4;
        tstep = (tnext - t) >> 4;
      } else {
        // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
        // can't step off polygon), clamp, calculate s and t steps across
        // span by division, biasing steps low so we don't run off the
        // texture
        spancountminus1 = (float)(spancount - 1);
        sdivz += st->sdivzstepu * spancountminus1;
        tdivz += st->tdivzstepu * spancountminus1;
        zi += st->zistepu * spancountminus1;
        z = (float)0x10000 * R_Recip (zi); // prescale to 16.16 fixed-point
        snext = (int)(sdivz * z) + sadj;
        if (snext > bbs)
          snext = bbs;
        else if (snext < 16)
          snext = 16; // prevent round-off error on <0 steps from
                      //  from causing overstepping & running off the
                      //  edge of the texture

        tnext = (int)(tdivz * z) + tadj;
        if (tnext > bbt)
          tnext = bbt;
        else if (tnext < 16)
          tnext = 16; // guard against round-off error on <0 steps

        if (spancount > 1) {
          sstep = (snext - s) / (spancount - 1);
          tstep = (tnext - t) / (spancount - 1);
        }
      }

      s = s & ((CYCLE << 16) - 1);
      t = t & ((CYCLE << 16) - 1);

      // D_DrawTurbulent8Span
      do {
        int sturb = ((s + turb[(t >> 16) & (CYCLE - 1)]) >> 16) & 63;
        int tturb = ((t + turb[(s >> 16) & (CYCLE - 1)]) >> 16) & 63;
        *pdest++ = *(pbase + (tturb << 6) + sturb);
        s += sstep;
        t += tstep;
      } while (--spancount > 0);

      s = snext;
      t = tnext;

    } while (count > 0);

  } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans16

unrolled- mh, MK, qbism
=============
*/
#define WRITEPDEST(i) { pdest[i] = *(pbase + (s >> 16) + (t >> 16) * cachewidth); s+=sstep; t+=tstep;}

void IRAM_ATTR D_DrawSpans16(espan_t *pspan, const spanstate_t *st, int rows) //qb: up it from 8 to 16.  This + unroll = big speed gain!
{
	int          count, spancount;
	byte         *pbase, *pdest;
	fixed16_t    s, t, snext, tnext, sstep, tstep;
	float        sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float        sdivzstepu, tdivzstepu, zistepu;
	const int    cachewidth = st->cachewidth;
	const fixed16_t sadj = st->sadjust, tadj = st->tadjust;
	const fixed16_t bbs = st->bbextents, bbt = st->bbextentt;

	sstep = 0;   // keep compiler happy
	tstep = 0;   // ditto

	pbase = (byte *)st->cacheblock;
	sdivzstepu = st->sdivzstepu * 16;
	tdivzstepu = st->tdivzstepu * 16;
	zistepu = st->zistepu * 16;

	do
	{
		if (SKIPSPAN(pspan, rows))
			continue;
		pdest = (byte *)((byte *)d_viewbuffer + (r_screenwidth * pspan->v) + pspan->u);
		count = pspan->count >> 4;

		spancount = pspan->count % 16;

		// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = st->sdivzorigin + dv*st->sdivzstepv + du*st->sdivzstepu;
		tdivz = st->tdivzorigin + dv*st->tdivzstepv + du*st->tdivzstepu;
		zi = st->ziorigin + dv*st->zistepv + du*st->zistepu;
		z = (float)0x10000 * R_Recip (zi);   // prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadj;
		if (s < 0) s = 0;
		else if (s > bbs) s = bbs;

		t = (int)(tdivz * z) + tadj;
		if (t < 0) t = 0;
		else if (t > bbt) t = bbt;

		while (count-- > 0) // Manoel Kasimier
		{
			sdivz += sdivzstepu;
			tdivz += tdivzstepu;
			zi += zistepu;
			z = (float)0x10000 * R_Recip (zi);   // prescale to 16.16 fixed-point

			snext = (int)(sdivz * z) + sadj;
			if (snext < 16) snext = 16;
			else if (snext > bbs) snext = bbs;

			tnext = (int)(tdivz * z) + tadj;
			if (tnext < 16) tnext = 16;
			else if (tnext > bbt) tnext = bbt;

			sstep = (snext - s) >> 4;
			tstep = (tnext - t) >> 4;
			pdest += 16;

			WRITEPDEST(-16);
			WRITEPDEST(-15);
			WRITEPDEST(-14);
			WRITEPDEST(-13);
			WRITEPDEST(-12);
			WRITEPDEST(-11);
			WRITEPDEST(-10);
			WRITEPDEST(-9);
			WRITEPDEST(-8);
			WRITEPDEST(-7);
			WRITEPDEST(-6);
			WRITEPDEST(-5);
			WRITEPDEST(-4);
			WRITEPDEST(-3);
			WRITEPDEST(-2);
			WRITEPDEST(-1);

			s = snext;
			t = tnext;
		}
		if (spancount > 0)
		{
			spancountminus1 = (float)(spancount - 1);
			sdivz += st->sdivzstepu * spancountminus1;
			tdivz += st->tdivzstepu * spancountminus1;
			zi += st->zistepu * spancountminus1;
			z = (float)0x10000 * R_Recip (zi);   // prescale to 16.16 fixed-point

			snext = (int)(sdivz * z) + sadj;
			if (snext < 16) snext = 16;
			else if (snext > bbs) snext = bbs;

			tnext = (int)(tdivz * z) + tadj;
			if (tnext < 16) tnext = 16;
			else if (tnext > bbt) tnext = bbt;

			if (spancount > 1)
			{
				sstep = (snext - s) / (spancount - 1);
				tstep = (tnext - t) / (spancount - 1);
			}

			pdest += spancount;

			switch (spancount)
			{
			case 16:
				WRITEPDEST(-16);
			case 15:
				WRITEPDEST(-15);
			case 14:
				WRITEPDEST(-14);
			case 13:
				WRITEPDEST(-13);
			case 12:
				WRITEPDEST(-12);
			case 11:
				WRITEPDEST(-11);
			case 10:
				WRITEPDEST(-10);
			case  9:
				WRITEPDEST(-9);
			case  8:
				WRITEPDEST(-8);
			case  7:
				WRITEPDEST(-7);
			case  6:
				WRITEPDEST(-6);
			case  5:
				WRITEPDEST(-5);
			case  4:
				WRITEPDEST(-4);
			case  3:
				WRITEPDEST(-3);
			case  2:
				WRITEPDEST(-2);
			case  1:
				WRITEPDEST(-1);
				break;
			}
		}
	} while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawZSpans
=============
*/
void IRAM_ATTR D_DrawZSpans(espan_t *pspan, float ziorigin, float zistepu, float zistepv, int rows) {
  int count, quadcount, doublecount, izistep;
  int izi;
  short *pdest;
  unsigned ltemp;
  float zi;
  float du, dv;

  const int zwidth = d_zwidth;
  short *const pzbuffer = d_pzbuffer;

  izistep = (int)(zistepu * 0x8000 * 0x10000);

  do {
    if (SKIPSPAN(pspan, rows))
      continue;
    pdest = pzbuffer + (zwidth * pspan->v) + pspan->u;
    count = pspan->count;

    du = (float)pspan->u;
    dv = (float)pspan->v;
    zi = ziorigin + dv * zistepv + du * zistepu;
    izi = (int)(zi * 0x8000 * 0x10000);

    // align to 4 bytes
    if ((size_t)pdest & 0x02) {
      *pdest++ = (short)(izi >> 16);
      izi += izistep;
      count--;
    }

    // 4 pairs per iteration
    if ((quadcount = count >> 3) > 0) {
      do {
        ltemp  = (unsigned)izi >> 16;
        izi += izistep;
        ltemp |= (unsigned)izi & 0xFFFF0000u;
        izi += izistep;
        ((unsigned *)pdest)[0] = ltemp;

        ltemp  = (unsigned)izi >> 16;
        izi += izistep;
        ltemp |= (unsigned)izi & 0xFFFF0000u;
        izi += izistep;
        ((unsigned *)pdest)[1] = ltemp;

        ltemp  = (unsigned)izi >> 16;
        izi += izistep;
        ltemp |= (unsigned)izi & 0xFFFF0000u;
        izi += izistep;
        ((unsigned *)pdest)[2] = ltemp;

        ltemp  = (unsigned)izi >> 16;
        izi += izistep;
        ltemp |= (unsigned)izi & 0xFFFF0000u;
        izi += izistep;
        ((unsigned *)pdest)[3] = ltemp;

        pdest += 8;
      } while (--quadcount > 0);
    }

    count &= 7;

    // the remaining pairs (0 to 3)
    if ((doublecount = count >> 1) > 0) {
      do {
        ltemp  = (unsigned)izi >> 16;
        izi += izistep;
        ltemp |= (unsigned)izi & 0xFFFF0000u;
        izi += izistep;
        *(unsigned *)pdest = ltemp;
        pdest += 2;
      } while (--doublecount > 0);
    }

    if (count & 1)
      *pdest = (short)(izi >> 16);

  } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_FlatFillSpans

Simple single color fill with no texture mapping
=============
*/
void D_FlatFillSpans(espan_t *pspan, int color, int rows) {
  for (; pspan; pspan = pspan->pnext) {
    if (SKIPSPAN(pspan, rows))
      continue;
    memset((byte *)d_viewbuffer + r_screenwidth * pspan->v + pspan->u, color, pspan->count);
  }
}
