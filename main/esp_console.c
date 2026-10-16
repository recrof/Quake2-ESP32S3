/**
 * @file esp_console.c
 * @brief USB serial console of the ESP32-S3 Quake 2 port.
 *
 * Lines received on the USB serial port are Quake console commands (e.g.
 * "map base1", "god", "mem"), except for these local commands:
 *   shot               dumps the screen: "SHOT <w> <h>", then the 768 byte
 *                      palette and the 8-bit pixels, hex encoded, then "ENDSHOT"
 *   sysmem             free internal RAM and PSRAM
 *   tasks              unused stack of the tasks
 *   cpu                CPU use of each task over one second
 *   prof [ms]          sampling profile of the game core (tools/profile.py)
 *   ls <dir>           lists a directory of the SD card
 *   put <path> <size>  receives a file and writes it on the SD card (tools/sdput.py):
 *                      the data is sent in blocks of at most PUT_BLOCK bytes,
 *                      each acknowledged with "K <crc32>" once written
 *   reboot
 *   start              starts the game after a crash (see app_main)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "soc/assist_debug_reg.h"
#include "soc/soc.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_video.h"
#include "esp_console.h"

#define CONSOLE_TASK_PRIORITY   2
#define CONSOLE_TASK_CORE       0
#define MAX_LINE                128
#define COMMAND_QUEUE_LENGTH    8
#define PUT_BLOCK               16384

static QueueHandle_t commandQueue;      // lines for the engine
static volatile bool startRequested;
static char engineLine[MAX_LINE];

static void writeAll(const void *data, size_t len)
{
    const char *p = data;
    while (len)
    {
        int n = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(1000));
        if (n <= 0)
            return;
        p += n;
        len -= n;
    }
}

static void dumpHex(const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    char line[129];
    while (len)
    {
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++)
        {
            line[2 * i] = hex[data[i] >> 4];
            line[2 * i + 1] = hex[data[i] & 15];
        }
        line[2 * n] = '\n';
        writeAll(line, 2 * n + 1);
        data += n;
        len -= n;
    }
}

static void screenshot(void)
{
    uint8_t palette[768];
    const uint8_t *pixels = videoGetFrame();
    videoGetPalette(palette);
    flockfile(stdout);
    fflush(stdout);
    writeAll("SHOT 320 240\n", 13);
    dumpHex(palette, sizeof(palette));
    dumpHex(pixels, 320 * 240);
    writeAll("ENDSHOT\n", 8);
    funlockfile(stdout);
}

static int readExact(uint8_t *buf, int len, int timeoutMs)
{
    int got = 0;
    int64_t deadline = esp_timer_get_time() + timeoutMs * 1000LL;
    while (got < len && esp_timer_get_time() < deadline)
    {
        int n = usb_serial_jtag_read_bytes(buf + got, len - got, pdMS_TO_TICKS(100));
        if (n > 0)
            got += n;
    }
    return got;
}

static void mkdirs(const char *path)
{
    char tmp[MAX_LINE];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

static void putFile(const char *path, long size)
{
    uint8_t *buf = heap_caps_malloc(PUT_BLOCK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf)
        buf = malloc(PUT_BLOCK);
    mkdirs(path);
    FILE *f = buf ? fopen(path, "wb") : NULL;
    if (!f)
    {
        printf("E cannot create %s\n", path);
        free(buf);
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);
    printf("READY\n");
    fflush(stdout);
    long done = 0;
    int64_t start = esp_timer_get_time();
    while (done < size)
    {
        int n = size - done > PUT_BLOCK ? PUT_BLOCK : size - done;
        if (readExact(buf, n, 5000) != n)
        {
            printf("E timeout at %ld\n", done);
            break;
        }
        if (fwrite(buf, 1, n, f) != (size_t) n)
        {
            printf("E write failed at %ld\n", done);
            break;
        }
        done += n;
        printf("K %08lx\n", (unsigned long) esp_rom_crc32_le(0, buf, n));
        fflush(stdout);
    }
    fclose(f);
    free(buf);
    if (done == size)
    {
        int64_t us = esp_timer_get_time() - start;
        printf("DONE %ld bytes, %d kB/s\n", size, (int) ((size / 1024) * 1000000LL / (us ? us : 1)));
    }
    fflush(stdout);
}

static void listDir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
    {
        printf("cannot open %s\n", path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)))
    {
        char full[MAX_LINE + 300];      // path and a FAT name (255)
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (!stat(full, &st))
            printf("%10ld %s%s\n", (long) st.st_size, e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
    }
    closedir(d);
}

static void execute(char *line)
{
    char copy[MAX_LINE];
    strlcpy(copy, line, sizeof(copy));
    char *argv[4];
    int argc = 0;
    for (char *tok = strtok(copy, " \t\r\n"); tok && argc < 4; tok = strtok(NULL, " \t\r\n"))
        argv[argc++] = tok;
    if (!argc)
        return;
    if (!strcmp(argv[0], "shot"))
        screenshot();
    else if (!strcmp(argv[0], "put") && argc == 3)
        putFile(argv[1], atol(argv[2]));
    else if (!strcmp(argv[0], "ls"))
        listDir(argc > 1 ? argv[1] : "/sd");
    else if (!strcmp(argv[0], "reboot"))
        esp_restart();
    else if (!strcmp(argv[0], "start"))
        startRequested = true;
    else if (!strcmp(argv[0], "sysmem"))
    {
        printf("internal: free %u, min %u, largest %u; PSRAM: free %u, min %u, largest %u\n",
            (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
            (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    else if (!strcmp(argv[0], "tasks"))
    {
        static const char *names[] = { "quake2", "display", "audio", "touch", "buttons", "blehid", "console" };
        for (int i = 0; i < (int) (sizeof(names) / sizeof(names[0])); i++)
        {
            TaskHandle_t t = xTaskGetHandle(names[i]);
            if (t)
                printf("%s: %u bytes of stack never used\n", names[i], (unsigned) uxTaskGetStackHighWaterMark(t));
        }
    }
    else if (!strcmp(argv[0], "prof"))
    {
        // sampling profiler: the PC of the game core (core 1), read through
        // the debug assist PC recording, printed as "PROF <pc> <pc>..." lines
        // (tools/profile.py makes the per function histogram)
        int ms = argc > 1 ? atoi(argv[1]) : 2000;
        int max = 16384, n = 0;
        uint32_t *pcs = heap_caps_malloc(max * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        if (!pcs)
        {
            printf("prof: no memory\n");
            return;
        }
        REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_PDEBUGENABLE_REG, 1);
        REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_RECORDING_REG, 1);
        int64_t end = esp_timer_get_time() + ms * 1000LL;
        while (n < max && esp_timer_get_time() < end)
        {
            pcs[n++] = REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
            esp_rom_delay_us(50);
            if ((n & 127) == 0)
                vTaskDelay(1);      // the idle task (task watchdog)
        }
        for (int i = 0; i < n; i += 16)
        {
            printf("PROF");
            for (int j = i; j < i + 16 && j < n; j++)
                printf(" %08lx", (unsigned long) pcs[j]);
            printf("\n");
        }
        printf("PROF END %d samples\n", n);
        free(pcs);
    }
    else if (!strcmp(argv[0], "cpu"))
    {
        // CPU use of each task over one second (FreeRTOS run time stats)
        static TaskStatus_t a[48], b[48];
        extern int64_t parallelWaitUs;
        uint32_t ta, tb;
        int64_t wait = parallelWaitUs;
        int na = uxTaskGetSystemState(a, 48, &ta);
        vTaskDelay(pdMS_TO_TICKS(1000));
        int nb = uxTaskGetSystemState(b, 48, &tb);
        printf("game core waited %d ms for the other one\n", (int) ((parallelWaitUs - wait) / 1000));
        extern int64_t submitWaitUs, submitCopyUs;
        static int64_t lastWait, lastCopy;
        printf("frame present: waited %d ms for the display, copy %d ms (since last time)\n",
            (int) ((submitWaitUs - lastWait) / 1000), (int) ((submitCopyUs - lastCopy) / 1000));
        lastWait = submitWaitUs;
        lastCopy = submitCopyUs;
        for (int i = 0; i < nb; i++)
        {
            for (int j = 0; j < na; j++)
            {
                if (a[j].xHandle == b[i].xHandle)
                {
                    uint32_t d = b[i].ulRunTimeCounter - a[j].ulRunTimeCounter;
                    if (d * 1000ULL / (tb - ta) >= 5)
                        printf("%-12s %5.1f%%\n", b[i].pcTaskName, d * 100.0 / (tb - ta));
                }
            }
        }
    }
    else
    {
        char *msg = strdup(line);
        if (msg && xQueueSend(commandQueue, &msg, 0) != pdTRUE)
            free(msg);
    }
}

static void consoleTask(void *arg)
{
    (void) arg;
    char line[MAX_LINE];
    int len = 0;
    while (1)
    {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1)
            continue;
        if (c == '\n' || c == '\r')
        {
            line[len] = 0;
            execute(line);
            len = 0;
        }
        else if (len < MAX_LINE - 1)
        {
            line[len++] = c;
        }
    }
}

void consoleInit(void)
{
    commandQueue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(char *));
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = PUT_BLOCK + 1024;
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK)
        return;
    // stdout through the driver too, so that both can be used
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(consoleTask, "console", 4096, NULL, CONSOLE_TASK_PRIORITY, NULL, CONSOLE_TASK_CORE);
}

bool consoleStartRequested(void)
{
    return startRequested;
}

// Engine interface: next console line, NULL if none (game task)
char *QG_ConsoleInput(void)
{
    char *msg;
    if (!commandQueue || xQueueReceive(commandQueue, &msg, 0) != pdTRUE)
        return NULL;
    strlcpy(engineLine, msg, sizeof(engineLine));
    free(msg);
    return engineLine;
}
