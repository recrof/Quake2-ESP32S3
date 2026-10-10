/**
 * @file esp_gt911.c
 * @brief Goodix GT911 touch controller (I2C), shared by the boards that have one.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include "esp_gt911.h"
#include "board_config.h"

#ifdef TOUCH_SWAP_XY
#include "esp_log.h"
#include "display.h"

#define GT911_ADDR              0x5D        // selected by holding INT low during reset
#define GT911_ADDR_ALT          0x14
#define GT911_I2C_FREQ_HZ       400000
// GT911 registers (16 bit addresses)
#define GT911_REG_CONFIG_XMAX   0x8048
#define GT911_REG_PRODUCT_ID    0x8140
#define GT911_REG_STATUS        0x814E
#define GT911_REG_POINTS        0x814F
#define GT911_POINT_SIZE        8

static const char *TAG = "gt911";
static i2c_master_dev_handle_t gt911;
static int touchMaxX, touchMaxY;        // touch controller resolution (native orientation)

static esp_err_t gt911Read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(gt911, addr, 2, data, len, 50);
}

static esp_err_t gt911Write8(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = { reg >> 8, reg & 0xFF, value };
    return i2c_master_transmit(gt911, buf, 3, 50);
}

bool gt911Init(i2c_master_bus_handle_t bus)
{
    uint8_t address = GT911_ADDR;
    if (i2c_master_probe(bus, address, 50) != ESP_OK)
    {
        address = GT911_ADDR_ALT;
        if (i2c_master_probe(bus, address, 50) != ESP_OK)
        {
            ESP_LOGE(TAG, "GT911 touch controller not found");
            return false;
        }
    }
    i2c_device_config_t cfg =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = GT911_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &gt911) != ESP_OK)
    {
        gt911 = NULL;
        return false;
    }
    char id[5] = { 0 };
    uint8_t res[4] = { 0 };
    gt911Read(GT911_REG_PRODUCT_ID, (uint8_t *) id, 4);
    gt911Read(GT911_REG_CONFIG_XMAX, res, 4);
    touchMaxX = res[0] | (res[1] << 8);
    touchMaxY = res[2] | (res[3] << 8);
    if (touchMaxX <= 0 || touchMaxY <= 0)
    {
        touchMaxX = TOUCH_NATIVE_WIDTH;
        touchMaxY = TOUCH_NATIVE_HEIGHT;
    }
    ESP_LOGI(TAG, "Touch controller GT%s at 0x%02x, %dx%d", id, address, touchMaxX, touchMaxY);
    return true;
}

bool gt911Present(void)
{
    return gt911 != NULL;
}

int gt911ReadPoints(boardTouchPoint_t *points)
{
    uint8_t status;
    if (!gt911 || gt911Read(GT911_REG_STATUS, &status, 1) != ESP_OK || !(status & 0x80))
    {
        return -1;      // no new data
    }
    int n = status & 0x0F;
    if (n > BOARD_TOUCH_MAX_POINTS)
        n = BOARD_TOUCH_MAX_POINTS;
    uint8_t buf[BOARD_TOUCH_MAX_POINTS * GT911_POINT_SIZE];
    if (n && gt911Read(GT911_REG_POINTS, buf, n * GT911_POINT_SIZE) != ESP_OK)
    {
        n = -1;
    }
    gt911Write8(GT911_REG_STATUS, 0);
    for (int i = 0; i < n; i++)
    {
        const uint8_t *p = &buf[i * GT911_POINT_SIZE];
        int rx = p[1] | (p[2] << 8);
        int ry = p[3] | (p[4] << 8);
        // native (portrait) -> LCD landscape coordinates
#if TOUCH_SWAP_XY
        int x = ry * LCD_WIDTH / touchMaxY;
        int y = rx * LCD_HEIGHT / touchMaxX;
#else
        int x = rx * LCD_WIDTH / touchMaxX;
        int y = ry * LCD_HEIGHT / touchMaxY;
#endif
#if TOUCH_INVERT_X
        x = LCD_WIDTH - 1 - x;
#endif
#if TOUCH_INVERT_Y
        y = LCD_HEIGHT - 1 - y;
#endif
        points[i].id = p[0];
        points[i].x = x < 0 ? 0 : (x >= LCD_WIDTH ? LCD_WIDTH - 1 : x);
        points[i].y = y < 0 ? 0 : (y >= LCD_HEIGHT ? LCD_HEIGHT - 1 : y);
#if TOUCH_DEBUG
        ESP_LOGI(TAG, "touch id %d raw %d,%d -> %d,%d", p[0], rx, ry, points[i].x, points[i].y);
#endif
    }
    return n;
}
#endif
