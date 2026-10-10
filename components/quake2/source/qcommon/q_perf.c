/*
q_perf.c: per frame performance recorder.

	perf start [frames]		records the next frames (default 4096)
	perf stop				stops recording
	perf dump				prints the recorded frames as PERF lines

tools/perfgraph.py makes a graph of a dump (one stacked bar per frame).
*/
#include "qcommon.h"
#include "q_perf.h"

#define PERF_DEPTH		16
#define PERF_UNIT_US	10		// stored times: units of 10 us (max 655 ms per slot)
#define PERF_UNDRAWN_US	100000	// a record without a drawn frame (loading): every 100 ms

typedef struct
{
	unsigned short	slot[PERF_SLOTS];
	unsigned short	ev[PERF_EVENTS];
	unsigned short	serverframe;
} perfframe_t;

int		perf_active;

static perfframe_t	*perf_frames;
static int			perf_max, perf_count;
static int			perf_cur[PERF_SLOTS];	// current frame, us
static int			perf_ev[PERF_EVENTS];
static int			perf_stack[PERF_DEPTH];
static int			perf_depth;
static int			perf_last;				// time of the last push/pop
static int			perf_frame_start;
static int			perf_drawn;				// a frame was drawn since the last record

int QG_Microseconds (void);
extern int cl_serverframe_for_perf (void);

static void Perf_Charge (int now)
{
	perf_cur[perf_stack[perf_depth]] += now - perf_last;
	perf_last = now;
}

void Perf_PushSlot (int slot)
{
	if (!QG_IsGameTask ())
		return;
	Perf_Charge (QG_Microseconds ());
	if (perf_depth < PERF_DEPTH - 1)
		perf_depth++;
	perf_stack[perf_depth] = slot;
}

void Perf_PopSlot (void)
{
	if (!QG_IsGameTask ())
		return;
	Perf_Charge (QG_Microseconds ());
	if (perf_depth > 0)
		perf_depth--;
}

void Perf_CountEvent (int ev, int n)
{
	if (QG_IsGameTask ())
		perf_ev[ev] += n;
}

void Perf_FrameDrawn (void)
{
	perf_drawn = true;
}

int Perf_EventCount (int ev)
{
	return perf_ev[ev];
}

static void Perf_Reset (int now)
{
	memset (perf_cur, 0, sizeof(perf_cur));
	memset (perf_ev, 0, sizeof(perf_ev));
	perf_drawn = false;
	perf_depth = 0;			// an error (longjmp) may have left pushed slots
	perf_stack[0] = PERF_OTHER;
	perf_last = perf_frame_start = now;
}

void Perf_Frame (void)
{
	int		i, now;
	perfframe_t	*f;

	if (!perf_active)
		return;
	now = QG_Microseconds ();
	Perf_Charge (now);
	perf_depth = 0;			// an error (longjmp) may have left pushed slots
	// the main loop also runs when the client waits for its next frame
	// (cl_maxfps): those iterations are added to the next drawn frame
	if (!perf_drawn && now - perf_frame_start < PERF_UNDRAWN_US)
		return;
	perf_drawn = false;
	if (perf_count < perf_max)
	{
		f = &perf_frames[perf_count++];
		for (i = 0; i < PERF_SLOTS; i++)
		{
			int v = (perf_cur[i] + PERF_UNIT_US / 2) / PERF_UNIT_US;
			f->slot[i] = v > 65535 ? 65535 : v;
		}
		perf_ev[PERF_EV_KB] = (perf_ev[PERF_EV_KB] + 512) >> 10;	// counted in bytes
		for (i = 0; i < PERF_EVENTS; i++)
			f->ev[i] = perf_ev[i] > 65535 ? 65535 : perf_ev[i];
		f->serverframe = cl_serverframe_for_perf ();
	}
	if (perf_count >= perf_max)
	{
		perf_active = false;
		Com_Printf ("perf: %i frames recorded\n", perf_count);
	}
	Perf_Reset (now);
}

static void Perf_f (void)
{
	char	*cmd = Cmd_Argv (1);
	int		i, j;

	if (!strcmp (cmd, "start"))
	{
		int n = Cmd_Argc () > 2 ? atoi (Cmd_Argv (2)) : 4096;
		if (n < 16)
			n = 16;
		if (perf_frames)
			free (perf_frames);
		perf_frames = malloc (n * sizeof(perfframe_t));
		if (!perf_frames)
		{
			Com_Printf ("perf: no memory for %i frames\n", n);
			perf_max = perf_count = 0;
			return;
		}
		perf_max = n;
		perf_count = 0;
		Perf_Reset (QG_Microseconds ());
		perf_active = true;
		Com_Printf ("perf: recording %i frames\n", n);
	}
	else if (!strcmp (cmd, "stop"))
	{
		perf_active = false;
		Com_Printf ("perf: %i frames recorded\n", perf_count);
	}
	else if (!strcmp (cmd, "dump"))
	{
		perf_active = false;
		// header: unit, slot names, event names
		Com_Printf ("PERFH %i other server game client render parallel present file sound asset"
			" | opens kb sounds images\n", PERF_UNIT_US);
		for (i = 0; i < perf_count; i++)
		{
			perfframe_t *f = &perf_frames[i];
			char	line[160];
			int		len = snprintf (line, sizeof(line), "PERF %i %i", i, f->serverframe);
			for (j = 0; j < PERF_SLOTS; j++)
				len += snprintf (line + len, sizeof(line) - len, " %i", f->slot[j]);
			for (j = 0; j < PERF_EVENTS; j++)
				len += snprintf (line + len, sizeof(line) - len, " %i", f->ev[j]);
			Com_Printf ("%s\n", line);
		}
		Com_Printf ("PERFEND %i\n", perf_count);
	}
	else
		Com_Printf ("perf start [frames] | stop | dump (%i frames recorded%s)\n",
			perf_count, perf_active ? ", recording" : "");
}

void Perf_Init (void)
{
	Cmd_AddCommand ("perf", Perf_f);
}
