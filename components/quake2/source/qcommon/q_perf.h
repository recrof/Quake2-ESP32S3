#ifndef Q_PERF_H
#define Q_PERF_H

// Per drawn frame performance recorder ("perf" console command, tools/perfgraph.py).
// The time of a frame is split between slots: Perf_Push charges the time
// since the last push/pop to the current slot and makes another one current,
// Perf_Pop goes back. Only the game task records (the other tasks are ignored).

enum
{
	PERF_OTHER,			// main loop, console, everything not below
	PERF_SERVER,		// SV_Frame without the game
	PERF_GAME,			// game logic (ge->RunFrame)
	PERF_CLIENT,		// CL_Frame without the rendering
	PERF_RENDER,		// R_RenderFrame and 2D drawing
	PERF_PARALLEL,		// waiting for the second core (QG_Parallel2)
	PERF_PRESENT,		// waiting for the display to take the frame
	PERF_FILE,			// file opens, seeks and reads (SD card)
	PERF_SOUND,			// sound loading, without the file reads
	PERF_ASSET,			// image and model loading, without the file reads
	PERF_SLOTS
};

// events counted in the frame
enum
{
	PERF_EV_OPENS,		// files opened
	PERF_EV_KB,			// kB read (counted in bytes)
	PERF_EV_SOUNDS,		// sounds loaded
	PERF_EV_IMAGES,		// images or models loaded
	PERF_EVENTS
};

extern int perf_active;

void Perf_Init (void);
void Perf_Frame (void);				// start of a frame (Qcommon_Frame)
void Perf_PushSlot (int slot);
void Perf_PopSlot (void);
void Perf_CountEvent (int ev, int n);
int Perf_EventCount (int ev);			// in the current frame
void Perf_FrameDrawn (void);			// a record is made per drawn frame

#define Perf_Push(slot)		do { if (perf_active) Perf_PushSlot (slot); } while (0)
#define Perf_Pop()			do { if (perf_active) Perf_PopSlot (); } while (0)
#define Perf_Count(ev, n)	do { if (perf_active) Perf_CountEvent (ev, n); } while (0)

// port: true on the task running the engine
int QG_IsGameTask (void);

#endif
