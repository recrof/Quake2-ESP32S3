/**
 * @file esp_gt911.h
 * @brief Goodix GT911 touch controller (I2C), shared by the boards that have one.
 * The coordinates are mapped to the landscape LCD with TOUCH_SWAP_XY,
 * TOUCH_INVERT_X and TOUCH_INVERT_Y (board_config.h).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#ifndef ESP_GT911_H
#define ESP_GT911_H
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_board.h"

// Finds the controller (address 0x5D or 0x14) once it is out of reset.
bool gt911Init(i2c_master_bus_handle_t bus);
bool gt911Present(void);
// Same as boardTouchRead()
int gt911ReadPoints(boardTouchPoint_t *points);
#endif
