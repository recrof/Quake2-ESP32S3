// Host build: ESP-IDF heap_caps on top of malloc
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_32BIT    (1 << 1)
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void) caps; return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void) caps; return calloc(n, size); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void) caps; return 0; }
// Q2MEM=<bytes> emulates the PSRAM left for the engine on the ESP32-S3
static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{
	(void) caps;
	const char *m = getenv("Q2MEM");
	return m ? (size_t) atoi(m) : 64 * 1024 * 1024;
}
