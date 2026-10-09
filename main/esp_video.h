/**
 * @file esp_video.h
 * @brief Frame output and boot text console (ESP32-S3 Quake 2 port).
 */
#ifndef ESP_VIDEO_H
#define ESP_VIDEO_H
#include <stdint.h>
void videoInit(void);
// Prints a line on the boot console (and on the serial console)
void videoPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Switches back to the text console (fatal errors)
void videoConsoleMode(void);
float videoTakeFps(void);
const uint8_t *videoGetFrame(void);
void videoGetPalette(uint8_t pal[768]);
// Engine interface
void QG_SetPalette(unsigned char pal[768]);
void QG_DrawFrame(void *pixels);
#endif
