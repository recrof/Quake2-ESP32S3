/**
 * @file esp_main.c
 * @brief Entry point of the ESP32-S3 Quake 2 port.
 *
 * Core usage:
 *  - core 1: the game (client, server, game logic, software renderer).
 *  - core 0: display refresh, audio, touch, Bluetooth, serial console.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "board_config.h"
#include "esp_board.h"
#include "display.h"
#include "esp_video.h"
#include "esp_input.h"
#include "esp_console.h"
#include "quake2.h"

#define GAME_TASK_STACK_SIZE    (32 * 1024)
#define GAME_TASK_PRIORITY      5
#define GAME_TASK_CORE          1
#define STATS_INTERVAL_US       5000000

static const char *TAG = "quake2";

void bleHidInit(void);
void Mem_Report(void);

int QG_Milliseconds(void)
{
    return (int) (esp_timer_get_time() / 1000);
}

int QG_Microseconds(void)
{
    return (int) esp_timer_get_time();
}

// Second half of the parallel work (QG_Parallel2) on core 0: the renderer
// draws the even rows on core 1 and the odd ones here
#define WORKER_TASK_CORE        0
#define WORKER_TASK_PRIORITY    GAME_TASK_PRIORITY
#define WORKER_TASK_STACK_SIZE  4096

static SemaphoreHandle_t workerGo, workerDone;
extern int64_t parallelWaitUs;
static void (*workerFn)(void *arg, int part);
static void *workerArg;

static void workerTask(void *arg)
{
    (void) arg;
    while (1)
    {
        xSemaphoreTake(workerGo, portMAX_DELAY);
        workerFn(workerArg, 1);
        xSemaphoreGive(workerDone);
    }
}

static void workerInit(void)
{
    workerGo = xSemaphoreCreateBinary();
    workerDone = xSemaphoreCreateBinary();
    if (!workerGo || !workerDone || xTaskCreatePinnedToCore(workerTask, "worker", WORKER_TASK_STACK_SIZE, NULL,
        WORKER_TASK_PRIORITY, NULL, WORKER_TASK_CORE) != pdPASS)
    {
        ESP_LOGW(TAG, "No worker task, rendering on one core");
        workerGo = NULL;
    }
}

void QG_Parallel2(void (*fn)(void *arg, int part), void *arg)
{
    if (!workerGo)
    {
        fn(arg, 0);
        fn(arg, 1);
        return;
    }
    workerFn = fn;
    workerArg = arg;
    xSemaphoreGive(workerGo);
    fn(arg, 0);
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(workerDone, portMAX_DELAY);
    parallelWaitUs += esp_timer_get_time() - t0;
}

// time the game core waited for the other one (profiling, "cpu" command)
int64_t parallelWaitUs;

void QG_Quit(void)
{
    esp_restart();
}

void QG_FatalError(const char *message)
{
    videoConsoleMode();
    videoPrintf("Error:");
    // wrap the message on the 40 column console
    for (const char *p = message; *p; )
    {
        char line[41];
        int n = strlen(p) > 40 ? 40 : strlen(p);
        memcpy(line, p, n);
        line[n] = 0;
        videoPrintf("%s", line);
        p += n;
    }
    videoPrintf("Halted. Press RESET to restart.");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// The frame buffer of the renderer, in internal RAM (every pixel is written
// there): reserved before Bluetooth takes its share
static void *screenBuffer;

void *QG_ScreenBuffer(int size)
{
    return size == 320 * 240 ? screenBuffer : NULL;
}

static void haltWithMessage(const char *msg)
{
    videoPrintf("%s", msg);
    videoPrintf("Halted.");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t mountSdCard(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mountConfig =
    {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    esp_err_t ret;
#if SD_MODE == SD_MODE_SPI
    if (SD_PIN_MOSI < 0 || SD_PIN_MISO < 0 || SD_PIN_SCLK < 0 || SD_PIN_CS < 0)
    {
        ESP_LOGE(TAG, "SD pins not configured, see board_config.h");
        return ESP_ERR_INVALID_ARG;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_CLOCK_KHZ;
    if (SD_SPI_HOST != DISPLAY_SPI_HOST)
    {
        spi_bus_config_t bus =
        {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16384,
        };
        ret = spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = SD_SPI_HOST;
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mountConfig, &card);
    if (ret != ESP_OK && host.max_freq_khz > SDMMC_FREQ_DEFAULT)
    {
        ESP_LOGW(TAG, "SD mount failed at %d kHz (%s), retrying at %d kHz", host.max_freq_khz, esp_err_to_name(ret), SDMMC_FREQ_DEFAULT);
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mountConfig, &card);
    }
#else
    if (SD_PIN_MOSI < 0 || SD_PIN_MISO < 0 || SD_PIN_SCLK < 0)
    {
        ESP_LOGE(TAG, "SD pins not configured, see board_config.h");
        return ESP_ERR_INVALID_ARG;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SD_SDMMC_CLOCK_KHZ;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_PIN_SCLK;
    slot.cmd = SD_PIN_MOSI;
    slot.d0 = SD_PIN_MISO;
#if SD_MODE == SD_MODE_SDMMC_4BIT
    slot.width = 4;
    slot.d1 = SD_PIN_D1;
    slot.d2 = SD_PIN_D2;
    slot.d3 = SD_PIN_D3;
#else
    slot.width = 1;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mountConfig, &card);
#endif
    if (ret == ESP_OK)
    {
        sdmmc_card_print_info(stdout, card);
    }
    return ret;
}

// Maps the flash asset partition (models and skins used in place, see
// tools/mkassets.py)
static void mapAssets(void)
{
    extern int Q_Assets_Init(const void *base, int size);
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "assets");
    const void *ptr;
    esp_partition_mmap_handle_t handle;
    if (!part || esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK)
    {
        videoPrintf("No flash asset partition");
        return;
    }
    if (!Q_Assets_Init(ptr, part->size))
    {
        videoPrintf("Flash assets not written: see README");
        esp_partition_munmap(handle);
    }
}

static void gameTask(void *arg)
{
    (void) arg;
    static char *argv[] =
    {
        "quake2",
        "+set", "basedir", SD_MOUNT_POINT,
        "+set", "cl_run", "1",          // always run (touch stick, gamepads)
        NULL
    };
    Quake2_Init(sizeof(argv) / sizeof(argv[0]) - 1, argv);
    int oldtime = QG_Milliseconds();
    int64_t statsTime = esp_timer_get_time();
    videoTakeFps();
    while (1)
    {
        inputPoll();
        int newtime, time;
        do
        {
            newtime = QG_Milliseconds();
            time = newtime - oldtime;
        } while (time < 1);
        oldtime = newtime;
        Quake2_Frame(time);
        if (esp_timer_get_time() - statsTime >= STATS_INTERVAL_US)
        {
            statsTime = esp_timer_get_time();
            printf("fps: %.1f\n", videoTakeFps());
        }
    }
}

void app_main(void)
{
    consoleInit();
    boardInit();
    displayInit();
    videoInit();
    videoPrintf("Quake II on ESP32-S3");
    videoPrintf("Build %s %s", __DATE__, __TIME__);
    boardBacklightOn();
    if (!esp_psram_is_initialized())
    {
        haltWithMessage("PSRAM not found: it is required!");
    }
    videoPrintf("PSRAM: %u kB free, internal RAM: %u kB free", (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    videoPrintf("Mounting SD card (detect pin %d)...", boardSdCardDetect());
    esp_err_t ret = mountSdCard();
    if (ret != ESP_OK)
    {
        videoPrintf("SD card error: %s", esp_err_to_name(ret));
        videoPrintf("Insert a FAT SD card with baseq2/pak0.pak");
        // wait for a card
        while ((ret = mountSdCard()) != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        videoPrintf("SD card mounted");
    }
    struct stat st;
    if (stat(SD_MOUNT_POINT "/baseq2/pak0.pak", &st))
    {
        videoPrintf("No baseq2/pak0.pak on the SD card.");
        videoPrintf("Copy it (and pak1.pak, pak2.pak) or");
        videoPrintf("upload it with tools/sdput.py.");
        haltWithMessage("");
    }
    // after a crash, wait instead of crashing again and again (a boot loop keeps
    // the USB port from being used for flashing): "start" on the serial console
    // or RESET starts the game
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
    {
        videoPrintf("The game crashed (reset reason %d).", reason);
        videoPrintf("Press RESET to restart.");
        while (!consoleStartRequested())
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    mapAssets();
    screenBuffer = heap_caps_malloc(320 * 240, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    inputInit();
#if BLE_HID_ENABLED
    videoPrintf("Starting Bluetooth LE HID host...");
    bleHidInit();
#endif
    videoPrintf("Free RAM: internal %u kB, PSRAM %u kB", (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    workerInit();
    videoPrintf("Starting Quake II...");
    // The stack should be in internal RAM for speed, but PSRAM works too
    // (measured: 0.4 ms per frame, the stack being cached; Bluetooth needs
    // the internal RAM more)
    if (xTaskCreatePinnedToCoreWithCaps(gameTask, "quake2", GAME_TASK_STACK_SIZE, NULL, GAME_TASK_PRIORITY, NULL,
        GAME_TASK_CORE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS)
    {
        ESP_LOGW(TAG, "Not enough internal RAM for the game stack, using PSRAM");
        xTaskCreatePinnedToCoreWithCaps(gameTask, "quake2", GAME_TASK_STACK_SIZE, NULL, GAME_TASK_PRIORITY, NULL,
            GAME_TASK_CORE, MALLOC_CAP_SPIRAM);
    }
}
