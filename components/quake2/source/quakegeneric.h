#ifndef QUAKEGENERIC_H
#define QUAKEGENERIC_H


int QG_Milliseconds(void);
void QG_GetMouseDiff(int* dx, int* dy);
void QG_CaptureMouse(void);
void QG_ReleaseMouse(void);

void QG_Mkdir(const char* path);

// ESP32: data stored in its in-memory format in a mapped flash partition
// (models and decoded skins, see tools/mkassets.py), NULL if not there
const void *QG_FindAsset(const char *name, int *size);

// ESP32: runs fn(arg, 0) on the calling core and fn(arg, 1) on the other one,
// returns when both are done (one after the other where there is no second
// core)
void QG_Parallel2(void (*fn)(void *arg, int part), void *arg);

#endif