#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ST7789 320x240 SPI 屏驱动层（横屏模式；硬件/工具/Lua 共用的唯一入口）
 * 引脚配置在本文件内修改 */

#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240

/**
 * 初始化屏幕（幂等）
 */
esp_err_t screen_init(void);

/**
 * 整屏填充颜色（RGB565）
 */
esp_err_t screen_fill(uint16_t color);

/**
 * 在指定区域显示 RGB565 位图
 * @param x,y    起始坐标
 * @param w,h    宽高（像素）
 * @param rgb565 像素数据（RGB565，长度 w*h）
 */
esp_err_t screen_show_bitmap(int x, int y, int w, int h, const uint16_t *rgb565);

/**
 * 背光开关
 */
void screen_backlight(bool on);

#ifdef __cplusplus
}
#endif
