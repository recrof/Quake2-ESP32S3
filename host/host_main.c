// Host (macOS/Linux) build of the ESP32-S3 Quake 2 port, for testing the
// engine changes and measuring memory use without flashing the board.
//
// usage: q2host [-headless] [-frames N] [quake2 args, e.g. +map base1]
//
// The 320x240 8-bit frame is shown scaled 3x in an SDL window (unless
// -headless). Keyboard and mouse work as on a PC.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <SDL.h>

#include "quake2.h"
#include "client/keys.h"
#include "client/snd_loc.h"

#define SCALE 3

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t palette32[256];
static uint32_t rgb[320 * 240];
static int headless;
static int mouse_dx, mouse_dy;
static int max_frames = -1;
static int shot_frame = -1;       // -shot N FILE: saves frame N
static const char *shot_file;           // -msec N: fixed frame time (deterministic runs)

// no second core here: the two halves one after the other
void QG_Parallel2(void (*fn)(void *arg, int part), void *arg)
{
	fn(arg, 0);
	fn(arg, 1);
}

int QG_IsGameTask(void)
{
	return 1;
}

int QG_Microseconds(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int)(tv.tv_sec * 1000000LL + tv.tv_usec);
}

static int fixed_msec;
static int fixed_time;	// -msec: the clock advances by msec per frame (simulated time)

int QG_Milliseconds(void)
{
	static struct timeval start;
	if (fixed_msec)
		return fixed_time;
	struct timeval now;
	if (!start.tv_sec)
		gettimeofday(&start, NULL);
	gettimeofday(&now, NULL);
	return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
}

void QG_SetPalette(unsigned char palette[768])
{
	for (int i = 0; i < 256; i++)
		palette32[i] = 0xff000000u | (palette[i * 3] << 16) | (palette[i * 3 + 1] << 8) | palette[i * 3 + 2];
}

unsigned char host_last_frame[320 * 240];

void QG_DrawFrame(void *pixels)
{
	memcpy(host_last_frame, pixels, sizeof(host_last_frame));
	if (headless)
		return;
	const unsigned char *p = pixels;
	for (int i = 0; i < 320 * 240; i++)
		rgb[i] = palette32[p[i]];
	SDL_UpdateTexture(texture, NULL, rgb, 320 * 4);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);
}

// Saves the last frame as a PPM (console command "hostshot <file>")
void Host_SaveShot(const char *name)
{
	FILE *f = fopen(name, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n320 240\n255\n");
	for (int i = 0; i < 320 * 240; i++) {
		uint32_t c = palette32[host_last_frame[i]];
		unsigned char b[3] = { c >> 16, c >> 8, c };
		fwrite(b, 1, 3, f);
	}
	fclose(f);
}

static void Host_Shot_f(void)
{
	extern char *Cmd_Argv(int arg);
	Host_SaveShot(Cmd_Argv(1)[0] ? Cmd_Argv(1) : "shot.ppm");
}

static int map_key(SDL_Keycode k)
{
	switch (k) {
	case SDLK_TAB: return K_TAB;
	case SDLK_RETURN: return K_ENTER;
	case SDLK_ESCAPE: return K_ESCAPE;
	case SDLK_SPACE: return K_SPACE;
	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_UP: return K_UPARROW;
	case SDLK_DOWN: return K_DOWNARROW;
	case SDLK_LEFT: return K_LEFTARROW;
	case SDLK_RIGHT: return K_RIGHTARROW;
	case SDLK_LALT: case SDLK_RALT: return K_ALT;
	case SDLK_LCTRL: case SDLK_RCTRL: return K_CTRL;
	case SDLK_LSHIFT: case SDLK_RSHIFT: return K_SHIFT;
	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;
	case SDLK_F8: return K_F8;
	case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10;
	case SDLK_F11: return K_F11;
	case SDLK_F12: return K_F12;
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_HOME: return K_HOME;
	case SDLK_END: return K_END;
	case SDLK_INSERT: return K_INS;
	case SDLK_DELETE: return K_DEL;
	default:
		if (k >= 32 && k < 127)
			return k;
	}
	return 0;
}

static void poll_events(void)
{
	SDL_Event e;
	if (headless)
		return;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			exit(0);
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (!e.key.repeat) {
				int k = map_key(e.key.keysym.sym);
				if (k)
					Quake2_SendKey(k, e.type == SDL_KEYDOWN);
			}
			break;
		case SDL_MOUSEMOTION:
			mouse_dx += e.motion.xrel;
			mouse_dy += e.motion.yrel;
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			Quake2_SendKey(K_MOUSE1 + (e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2),
				e.type == SDL_MOUSEBUTTONDOWN);
			break;
		}
	}
}

int QG_GetKey(int *down, int *key)
{
	return 0;
}

void QG_GetMouseMove(int *x, int *y)
{
	*x = mouse_dx;
	*y = mouse_dy;
	mouse_dx = mouse_dy = 0;
}

void QG_CaptureMouse(void)
{
	if (!headless)
		SDL_SetRelativeMouseMode(SDL_TRUE);
}

void QG_ReleaseMouse(void)
{
	if (!headless)
		SDL_SetRelativeMouseMode(SDL_FALSE);
}

// ---------------------------------------------------------------------------
// Sound: the mixer runs into a ring buffer, consumed by the SDL audio callback
// ---------------------------------------------------------------------------
#define SND_BUFFER_SIZE (16 * 1024)
static unsigned char snd_buffer[SND_BUFFER_SIZE];
static volatile int snd_pos;    // bytes
static int snd_ok;

static void audio_callback(void *u, Uint8 *stream, int len)
{
	for (int i = 0; i < len; i++) {
		stream[i] = snd_buffer[snd_pos];
		snd_pos = (snd_pos + 1) & (SND_BUFFER_SIZE - 1);
	}
}

qboolean SNDDMA_Init(void)
{
	SDL_AudioSpec want = { 0 }, have;
	if (headless) {
		// no audio device: the play position follows the time, and
		// SNDDMA_Submit prints the peak of the mixed samples (sound check)
		dma.samplebits = 16;
		dma.speed = 11025;
		dma.channels = 2;
		dma.samples = SND_BUFFER_SIZE / 2;
		dma.samplepos = 0;
		dma.submission_chunk = 1;
		dma.buffer = snd_buffer;
		return true;
	}
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
		return false;
	want.freq = 11025;
	want.format = AUDIO_S16LSB;
	want.channels = 2;
	want.samples = 512;
	want.callback = audio_callback;
	if (SDL_OpenAudio(&want, &have) < 0)
		return false;
	dma.samplebits = 16;
	dma.speed = 11025;
	dma.channels = 2;
	dma.samples = SND_BUFFER_SIZE / 2;
	dma.samplepos = 0;
	dma.submission_chunk = 1;
	dma.buffer = snd_buffer;
	snd_ok = 1;
	SDL_PauseAudio(0);
	return true;
}

int SNDDMA_GetDMAPos(void)
{
	if (headless)
		snd_pos = (int)((long long)SDL_GetTicks() * 11025 * 4 / 1000) & (SND_BUFFER_SIZE - 1);
	dma.samplepos = snd_ok || headless ? snd_pos / 2 : 0;
	return dma.samplepos;
}

void SNDDMA_Shutdown(void)
{
	if (snd_ok)
		SDL_CloseAudio();
	snd_ok = 0;
}

void SNDDMA_BeginPainting(void)
{
	SDL_LockAudio();
}

void SNDDMA_Submit(void)
{
	if (headless) {
		static int calls, peak;
		short *b = (short *)snd_buffer;
		for (int i = 0; i < SND_BUFFER_SIZE / 2; i++)
			if (abs(b[i]) > peak)
				peak = abs(b[i]);
		if (++calls % 50 == 0) {
			printf("sound peak %d\n", peak); fflush(stdout);
			peak = 0;
		}
		return;
	}
	SDL_UnlockAudio();
}

void Mem_Report(void);

int main(int argc, char **argv)
{
	char *qargv[64];
	int qargc = 0;
	qargv[qargc++] = "quake2";
	qargv[qargc++] = "+set";
	qargv[qargc++] = "basedir";
	qargv[qargc++] = getenv("Q2BASEDIR") ? getenv("Q2BASEDIR") : ".";
	for (int i = 1; i < argc && qargc < 62; i++) {
		if (!strcmp(argv[i], "-headless"))
			headless = 1;
		else if (!strcmp(argv[i], "-shot") && i + 2 < argc) {
			shot_frame = atoi(argv[++i]);
			shot_file = argv[++i];
		}
		else if (!strcmp(argv[i], "-msec") && i + 1 < argc)
			fixed_msec = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-frames") && i + 1 < argc)
			max_frames = atoi(argv[++i]);
		else
			qargv[qargc++] = argv[i];
	}
	qargv[qargc] = NULL;
	// Q2ASSETS=<file>: flash asset image (tools/mkassets.py), as mapped on the ESP32
	if (getenv("Q2ASSETS")) {
		extern int Q_Assets_Init(const void *base, int size);
		FILE *f = fopen(getenv("Q2ASSETS"), "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long n = ftell(f);
			void *m = malloc(n);
			fseek(f, 0, SEEK_SET);
			fread(m, 1, n, f);
			fclose(f);
			Q_Assets_Init(m, n);
		}
	}
	if (!headless) {
		SDL_Init(SDL_INIT_VIDEO);
		window = SDL_CreateWindow("Quake 2 (ESP32-S3 host build)", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			320 * SCALE, 240 * SCALE, 0);
		renderer = SDL_CreateRenderer(window, -1, 0);
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
	}
	Quake2_Init(qargc, qargv);
	{
		extern void Cmd_AddCommand(char *name, void (*function)(void));
		Cmd_AddCommand("hostshot", Host_Shot_f);
	}
	int oldtime = Quake2_Milliseconds();
	for (int frame = 0; max_frames < 0 || frame < max_frames; frame++) {
		poll_events();
		if (fixed_msec) {
			fixed_time += fixed_msec;
			Quake2_Frame(fixed_msec);
		} else {
			int newtime, time;
			do {
				newtime = Quake2_Milliseconds();
				time = newtime - oldtime;
			} while (time < 1);
			oldtime = newtime;
			Quake2_Frame(time);
		}
		if (frame == shot_frame)
			Host_SaveShot(shot_file);
	}
	// a "perf start" capture is printed at the end of a -frames run
	extern void Cmd_ExecuteString(char *text);
	Cmd_ExecuteString("perf dump");
	Mem_Report();
	return 0;
}
