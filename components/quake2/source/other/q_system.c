
// Author: Alejandro Villegas Alonso
//         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
//
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../client/keys.h"
#include "../qcommon/qcommon.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "q_arena.h"

#include "../quakegeneric.h"

// Single global debug switch (menuconfig -> QUAKE2_ESP32P4_DEBUG).
#ifdef CONFIG_QUAKE2_ESP32P4_DEBUG
#define QUAKE2_ESP32P4_DEBUG
#endif

// All the engine memory lives in one arena (zone, model hunks, images,
// sounds, surface cache, file buffers): the largest PSRAM block, minus
// QUAKE_HEAP_RESERVE bytes left to the C heap. QUAKE_MEMSIZE > 0 caps it
// (the host build uses it to emulate the ESP32-S3 memory).
#ifndef QUAKE_MEMSIZE
#define QUAKE_MEMSIZE 0
#endif
#ifndef QUAKE_HEAP_RESERVE
#define QUAKE_HEAP_RESERVE (256 * 1024)
#endif

#define K_LAST 256
static unsigned char KeyStates[K_LAST];

void ProcessKeyEvent(int key, qboolean down);

struct {
  int key;
  int down;
} keyq[64];

int keyq_head = 0;
int keyq_tail = 0;

cvar_t *nostdout = NULL;

extern cvar_t *vid_fullscreen;

unsigned sys_frame_time;

qboolean stdin_active = false;

// =======================================================================
// General routines
// =======================================================================

void Sys_ConsoleOutput(char *string) {
  if (nostdout && nostdout->value)
    return;

  fputs(string, stdout);
}

void Sys_Printf(char *fmt, ...) {
  va_list argptr;
  char text[1024];
  unsigned char *p;

  va_start(argptr, fmt);
  vsprintf(text, fmt, argptr);
  va_end(argptr);

  if (strlen(text) > sizeof(text))
    Sys_Error("memory overwrite in Sys_Printf");

  if (nostdout && nostdout->value)
    return;

  for (p = (unsigned char *)text; *p; p++) {
    *p &= 0x7f;
    if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
      printf("[%02x]", *p);
    else
      putc(*p, stdout);
  }
}

// Platform hooks: what to do on quit and on fatal errors (default: exit)
__attribute__((weak)) void QG_Quit(void) { _Exit(0); }
__attribute__((weak)) void QG_FatalError(const char *message) { _Exit(1); }

void Sys_Quit(void) {
  CL_Shutdown();
  Qcommon_Shutdown();
  QG_Quit();
}

void Sys_Init(void) {
  memset(KeyStates, 0, sizeof(KeyStates));

#if id386
//	Sys_SetFPCW();
#endif
}

void Sys_Error(char *error, ...) {
  va_list argptr;
  char string[1024];

  // change stdin to non blocking
  // fcntl (0, F_SETFL, fcntl (0, F_GETFL, 0) & ~FNDELAY);

  CL_Shutdown();
  Qcommon_Shutdown();

  va_start(argptr, error);
  vsnprintf(string, sizeof(string), error, argptr);
  va_end(argptr);
  fprintf(stderr, "Error: %s\n", string);

  QG_FatalError(string);
}

void Sys_Warn(char *warning, ...) {
  va_list argptr;
  char string[1024];

  va_start(argptr, warning);
  vsprintf(string, warning, argptr);
  va_end(argptr);
  fprintf(stderr, "Warning: %s", string);
}

// Lines typed on the platform console (serial port), NULL if none
__attribute__((weak)) char *QG_ConsoleInput(void) { return NULL; }

char *Sys_ConsoleInput(void) { return QG_ConsoleInput(); }

/*****************************************************************************/

/*
=================
Sys_UnloadGame
=================
*/
void Sys_UnloadGame(void) {}

/*
=================
Sys_GetGameAPI

Loads the game dll
=================
*/
void *Sys_GetGameAPI(void *parms) {
  extern void *GetGameAPI(void *);
  return GetGameAPI(parms);
}

/*****************************************************************************/

void Sys_AppActivate(void) {}

void Sys_SendKeyEvents(void) {
#ifndef DEDICATED_ONLY
  static int reenterGuard = 0;

  if (reenterGuard == 0) {
    reenterGuard = 1;

    // Consume key queue
    while (keyq_head != keyq_tail) {
      ProcessKeyEvent(keyq[keyq_tail].key, keyq[keyq_tail].down);
      keyq_tail = (keyq_tail + 1) & 63;
    }

    reenterGuard = 0;
  }
#endif

  // grab frame time
  sys_frame_time = Sys_Milliseconds();
}

/*****************************************************************************/

char *Sys_GetClipboardData(void) { return NULL; }

void Sys_CopyProtect(void) {}

int curtime;
int Sys_Milliseconds(void) {
  curtime = QG_Milliseconds();
  return curtime;
}

void Sys_Mkdir(char *path) { QG_Mkdir(path); }

// File search ("dir/pattern", '*' wildcards in the file name only), used to
// wipe the level saves (SV_WipeSavegame) and to list the save slots
static DIR *find_dir;
static char find_base[MAX_OSPATH], find_pattern[MAX_OSPATH], find_path[MAX_OSPATH];

static qboolean Sys_WildMatch(const char *pattern, const char *s) {
  if (*pattern == '*')
    return Sys_WildMatch(pattern + 1, s) || (*s && Sys_WildMatch(pattern, s + 1));
  if (!*pattern)
    return !*s;
  return tolower((unsigned char)*pattern) == tolower((unsigned char)*s) && Sys_WildMatch(pattern + 1, s + 1);
}

char *Sys_FindNext(unsigned musthave, unsigned canthave) {
  struct dirent *d;

  if (!find_dir)
    return NULL;
  while ((d = readdir(find_dir)) != NULL) {
    qboolean isdir = d->d_type == DT_DIR;

    if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
      continue;
    if (!Sys_WildMatch(find_pattern, d->d_name))
      continue;
    if (((musthave & SFF_SUBDIR) && !isdir) || ((canthave & SFF_SUBDIR) && isdir))
      continue;
    Com_sprintf(find_path, sizeof(find_path), "%s/%s", find_base, d->d_name);
    return find_path;
  }
  return NULL;
}

char *Sys_FindFirst(char *path, unsigned musthave, unsigned canthave) {
  char *slash;

  if (find_dir)
    Sys_Error("Sys_FindFirst without close");
  Com_sprintf(find_base, sizeof(find_base), "%s", path);
  slash = strrchr(find_base, '/');
  if (slash) {
    *slash = 0;
    Com_sprintf(find_pattern, sizeof(find_pattern), "%s", slash + 1);
  } else {
    strcpy(find_base, ".");
    Com_sprintf(find_pattern, sizeof(find_pattern), "%s", path);
  }
  find_dir = opendir(find_base);
  return Sys_FindNext(musthave, canthave);
}

void Sys_FindClose(void) {
  if (find_dir)
    closedir(find_dir);
  find_dir = NULL;
}

void ProcessKeyEvent(int key, qboolean down) {
  if (key >= 0 && key < K_LAST) {
    KeyStates[key] = down ? 1 : 0;
  }

  Key_Event(key, down, Sys_Milliseconds());

  if (KeyStates[K_ALT] && key == K_ENTER && down) {
    Cvar_SetValue("vid_fullscreen", vid_fullscreen->value ? 0 : 1);
  }
}

// Memory use by category ("mem" console command)
void Mem_Report(void) {
  extern int z_bytes, z_count, r_image_bytes, snd_cache_bytes;
  Com_Printf("arena %d KB: used %d KB, peak %d KB, free %d KB (largest %d KB, %d blocks)\n",
             Q_Arena_Total() / 1024, Q_Arena_Used() / 1024, Q_Arena_Peak() / 1024,
             Q_Arena_FreeSize() / 1024, Q_Arena_Largest() / 1024, Q_Arena_Count());
  extern int SV_NumEdicts(void), CM_MemUsed(void), sc_size;
  Com_Printf("  collision map %d KB, surface cache %d KB\n", CM_MemUsed() / 1024, sc_size / 1024);
  Com_Printf("  zone %d KB (%d blocks, sounds %d KB), models %d KB, images %d KB, edicts %d\n",
             z_bytes / 1024, z_count, snd_cache_bytes / 1024, Hunk_PoolUsed() / 1024,
             r_image_bytes / 1024, SV_NumEdicts());
}

static void Arena_Dump_f(void) { Q_Arena_Dump(atoi(Cmd_Argv(1)) * 1024); }

void Quake2_Init(int argc, char **argv) {
  keyq_head = 0;
  keyq_tail = 0;
  memset(keyq, 0, sizeof(keyq));

  // Reserve the engine arena from PSRAM before any engine allocation.
  if (!Q_Arena_Init(QUAKE_MEMSIZE, QUAKE_HEAP_RESERVE))
    Sys_Error("Cannot reserve the engine memory");
  Hunk_Init();

  Qcommon_Init(argc, argv);
  {
    extern void Q_PurgeCaches(void);
    q_arena_purge = Q_PurgeCaches;
  }

  nostdout = Cvar_Get("nostdout", "0", 0);
  Cmd_AddCommand("mem", Mem_Report);
  Cmd_AddCommand("arena", Arena_Dump_f);
}

void Quake2_Frame(int msec) {
  Qcommon_Frame(msec);

#ifdef QUAKE2_ESP32P4_DEBUG
  static int frame_count;
  extern int z_bytes;

  if ((frame_count++ & 63) == 0) {
    ESP_LOGI(
        "MEM",
        "psram_free=%d psram_largest=%d internal_free=%d hunk_pool_used=%d "
        "z_bytes=%d arena_used=%d arena_free=%d arena_largest=%d",
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        Hunk_PoolUsed(), z_bytes, Q_Arena_Used(), Q_Arena_FreeSize(),
        Q_Arena_Largest());
  }
#endif
}

void ri_esp_log_model(const char *name, int filelen, int datasize) {
#ifdef QUAKE2_ESP32P4_DEBUG
  ESP_LOGI("MODEL", "%s file=%d data=%d", name, filelen, datasize);
#endif
}

int Quake2_Milliseconds() { return Sys_Milliseconds(); }

void Quake2_SendKey(int key, int down) {
  keyq[keyq_head].key = key;
  keyq[keyq_head].down = down;
  keyq_head = (keyq_head + 1) & 63;
}