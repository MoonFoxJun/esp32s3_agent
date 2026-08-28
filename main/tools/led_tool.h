#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 注册 WS2812B LED 工具（led_set）
 *
 * 接线后修改 led_tool.c 顶部的 LED_STRIP_GPIO / LED_STRIP_LED_NUM
 *
 * @return ESP_OK 成功
 */
esp_err_t led_tool_register(void);

#ifdef __cplusplus
}
#endif
