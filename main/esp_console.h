/**
 * @file esp_console.h
 * @brief USB serial console of the ESP32-S3 Quake 2 port (see esp_console.c).
 */
#ifndef ESP_CONSOLE_H
#define ESP_CONSOLE_H
#include <stdbool.h>
void consoleInit(void);
// "start" was received (after a crash, the game waits for it)
bool consoleStartRequested(void);
char *QG_ConsoleInput(void);
#endif
