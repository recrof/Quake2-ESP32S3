/**
 * @file esp_video.c
 * @brief Frame output for the ESP32-S3 Quake 2 port, and the boot text console.
 *
 * The software renderer draws 320x240 8-bit frames in internal RAM. At the end
 * of a frame the pixels are copied to a PSRAM front buffer and the game goes
 * on with the next frame, while the display task (core 0) converts the front
 * buffer to RGB565 through the palette, a few lines at a time, and sends it to
 * the LCD by DMA.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "display.h"
#include "esp_touch.h"
#include "esp_video.h"
#include "font8x8.h"
#include "qcommon/q_perf.h"

#define FRAME_WIDTH             320
#define FRAME_HEIGHT            240
#define LINES_PER_CHUNK         8
#define DISPLAY_TASK_PRIORITY   (configMAX_PRIORITIES - 2)
#define DISPLAY_TASK_CORE       0

static uint8_t *frontBuffer;                    // frame being sent (PSRAM)
static uint16_t palette[256];                   // RGB565, big endian (as sent to the LCD)
static uint16_t framePalette[256];              // palette of the front buffer
static uint16_t *lineBuffers[DISPLAY_MAX_QUEUED];
static TaskHandle_t displayTaskHandle;
static SemaphoreHandle_t frameDone;             // given when the front buffer can be overwritten
static volatile bool gameOverlay;               // draw the touch controls

// boot console
static uint8_t consoleLine;
static bool consoleActive = true;

static inline uint16_t rgb565be(int r, int g, int b)
{
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static void displayTask(void *arg)
{
    (void) arg;
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool overlay = gameOverlay && touchOverlayVisible();
        displaySetWindow(0, FRAME_HEIGHT);
        for (int y = 0, k = 0; y < FRAME_HEIGHT; y += LINES_PER_CHUNK, k++)
        {
            uint16_t *buffer = lineBuffers[k % DISPLAY_MAX_QUEUED];
            if (k >= DISPLAY_MAX_QUEUED)
            {
                displayWaitOne();       // the buffer we are going to fill must have been sent
            }
            for (int l = 0; l < LINES_PER_CHUNK; l++)
            {
                const uint32_t *src = (const uint32_t *) &frontBuffer[(y + l) * FRAME_WIDTH];
                uint32_t *dst = (uint32_t *) (buffer + l * FRAME_WIDTH);
                for (int i = 0; i < FRAME_WIDTH / 4; i++)
                {
                    uint32_t p = src[i];
                    dst[2 * i] = framePalette[p & 0xFF] | (framePalette[(p >> 8) & 0xFF] << 16);
                    dst[2 * i + 1] = framePalette[(p >> 16) & 0xFF] | (framePalette[p >> 24] << 16);
                }
                if (overlay)
                {
                    touchDrawOverlay(buffer + l * FRAME_WIDTH, y + l);
                }
            }
            displaySendPixels(buffer, LINES_PER_CHUNK * FRAME_WIDTH, y + LINES_PER_CHUNK >= FRAME_HEIGHT);
        }
        displayWaitAll();
        xSemaphoreGive(frameDone);
    }
}

void videoInit(void)
{
    frontBuffer = heap_caps_calloc(FRAME_WIDTH * FRAME_HEIGHT, 1, MALLOC_CAP_SPIRAM);
    if (!frontBuffer)
    {
        frontBuffer = heap_caps_calloc(FRAME_WIDTH * FRAME_HEIGHT, 1, MALLOC_CAP_8BIT);
    }
    for (int i = 0; i < DISPLAY_MAX_QUEUED; i++)
    {
        lineBuffers[i] = heap_caps_malloc(LINES_PER_CHUNK * FRAME_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(lineBuffers[i]);
    }
    // boot console colors: 0 black, 1 light grey, 2 white, 3 blue
    palette[0] = rgb565be(0, 0, 0);
    palette[1] = rgb565be(170, 170, 170);
    palette[2] = rgb565be(255, 255, 255);
    palette[3] = rgb565be(86, 119, 170);
    frameDone = xSemaphoreCreateBinary();
    xSemaphoreGive(frameDone);
    // stack in PSRAM: internal RAM is scarce, and this task never writes to flash
    xTaskCreatePinnedToCoreWithCaps(displayTask, "display", 3072, NULL, DISPLAY_TASK_PRIORITY, &displayTaskHandle,
        DISPLAY_TASK_CORE, MALLOC_CAP_SPIRAM);
}

// Sends a frame: returns once the pixels have been copied (the caller can draw the next one)
int64_t submitWaitUs, submitCopyUs;     // profiling ("cpu" command)

static void submitFrame(const uint8_t *pixels)
{
    int64_t t0 = esp_timer_get_time();
    Perf_Push(PERF_PRESENT);
    xSemaphoreTake(frameDone, portMAX_DELAY);
    int64_t t1 = esp_timer_get_time();
    if (pixels != frontBuffer)
    {
        memcpy(frontBuffer, pixels, FRAME_WIDTH * FRAME_HEIGHT);
    }
    Perf_Pop();
    submitWaitUs += t1 - t0;
    submitCopyUs += esp_timer_get_time() - t1;
    memcpy(framePalette, palette, sizeof(framePalette));
    xTaskNotifyGive(displayTaskHandle);
}

// ---------------------------------------------------------------------------
// Engine interface
// ---------------------------------------------------------------------------
void QG_SetPalette(unsigned char pal[768])
{
    for (int i = 0; i < 256; i++)
    {
        palette[i] = rgb565be(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
    }
}

static int fpsFrames;
static int64_t fpsStart;

void QG_DrawFrame(void *pixels)
{
    consoleActive = false;
    gameOverlay = true;
    submitFrame(pixels);
    fpsFrames++;
}

// Frames per second since the last call (serial console statistics)
float videoTakeFps(void)
{
    int64_t now = esp_timer_get_time();
    float fps = fpsStart ? fpsFrames * 1e6f / (now - fpsStart) : 0;
    fpsStart = now;
    fpsFrames = 0;
    return fps;
}

// Last frame, for screenshots (the front buffer is stable between frames)
const uint8_t *videoGetFrame(void)
{
    xSemaphoreTake(frameDone, portMAX_DELAY);
    xSemaphoreGive(frameDone);
    return frontBuffer;
}

void videoGetPalette(uint8_t pal[768])
{
    for (int i = 0; i < 256; i++)
    {
        uint16_t c = (framePalette[i] >> 8) | (framePalette[i] << 8);
        pal[i * 3] = ((c >> 11) & 0x1F) << 3;
        pal[i * 3 + 1] = ((c >> 5) & 0x3F) << 2;
        pal[i * 3 + 2] = (c & 0x1F) << 3;
    }
}

// ---------------------------------------------------------------------------
// Boot text console (before the game draws its first frame, and fatal errors)
// ---------------------------------------------------------------------------
void videoConsoleMode(void)
{
    xSemaphoreTake(frameDone, portMAX_DELAY);
    palette[0] = rgb565be(0, 0, 0);
    palette[1] = rgb565be(170, 170, 170);
    palette[2] = rgb565be(255, 255, 255);
    palette[3] = rgb565be(86, 119, 170);
    memset(frontBuffer, 0, FRAME_WIDTH * FRAME_HEIGHT);
    consoleLine = 0;
    consoleActive = true;
    gameOverlay = false;
    xSemaphoreGive(frameDone);
}

static void consolePutChar(char c, int x, int y, int color)
{
    for (int cy = 0; cy < 8; cy++)
    {
        uint8_t bits = font8x8_basic[0x7F & c][cy];
        uint8_t *dst = &frontBuffer[(y + cy) * FRAME_WIDTH + x];
        for (int cx = 0; cx < 8; cx++, bits >>= 1)
        {
            dst[cx] = (bits & 1) ? color : 0;
        }
    }
}

static void consolePrint(bool replace, const char *format, va_list va)
{
    char text[FRAME_WIDTH / 8 + 1];
    vsnprintf(text, sizeof(text), format, va);
    printf("%s\n", text);
    if (!consoleActive || !frontBuffer)
    {
        return;
    }
    xSemaphoreTake(frameDone, portMAX_DELAY);
    if (replace && consoleLine > 0)
    {
        consoleLine--;
    }
    if (consoleLine >= FRAME_HEIGHT / 8)
    {
        memmove(frontBuffer, frontBuffer + 8 * FRAME_WIDTH, (FRAME_HEIGHT - 8) * FRAME_WIDTH);
        consoleLine = FRAME_HEIGHT / 8 - 1;
    }
    memset(frontBuffer + consoleLine * 8 * FRAME_WIDTH, 0, 8 * FRAME_WIDTH);
    for (int i = 0; text[i] && i < FRAME_WIDTH / 8; i++)
    {
        consolePutChar(text[i], i * 8, consoleLine * 8, consoleLine == 0 ? 2 : 1);
    }
    consoleLine++;
    xSemaphoreGive(frameDone);
    if (displayIsReady())
    {
        submitFrame(frontBuffer);
    }
}

void videoPrintf(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    consolePrint(false, format, va);
    va_end(va);
}

void videoReplaceLine(const char *format, ...)
{
    va_list va;
    va_start(va, format);
    consolePrint(true, format, va);
    va_end(va);
}
