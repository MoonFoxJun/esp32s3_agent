#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WS2812B 灯条驱动层（硬件/工具/Lua 共用的唯一入口）
 * 硬件配置（GPIO、灯珠数）在本文件内修改 */

/**
 * 初始化灯条（幂等：只初始化一次）
 */
esp_err_t led_ctrl_init(void);

/**
 * 所有灯珠点亮为指定颜色
 */
esp_err_t led_ctrl_set_all(uint32_t r, uint32_t g, uint32_t b);

/**
 * 单颗灯珠写入缓冲（不刷新！动画请画完一帧后调 led_ctrl_refresh 统一送出，
 * 与 esp-claw 的 set_pixel + refresh 模式一致，避免逐颗刷新）
 * @param index 灯珠序号 0..led_count-1
 */
esp_err_t led_ctrl_set_pixel(int index, uint32_t r, uint32_t g, uint32_t b);

/**
 * 把缓冲中的像素一次性刷新到灯带
 */
esp_err_t led_ctrl_refresh(void);

/**
 * 所有灯珠熄灭
 */
esp_err_t led_ctrl_clear(void);

/**
 * 灯珠数量
 */
int led_ctrl_get_led_count(void);

#ifdef __cplusplus
}
#endif
