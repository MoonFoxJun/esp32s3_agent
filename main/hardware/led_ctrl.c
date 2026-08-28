#include "led_ctrl.h"

#include <stdint.h>

#include "driver/rmt_types.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led_ctrl";

/* ========== 硬件配置：接好线后修改这里 ==========
 * 注意：之前灯带显示混乱的根因是面包板走线串扰（LED 数据线与 TFT SPI 线
 * 平行导致），不是软件。走线分开后默认配置即可正常工作。 */
#define LED_STRIP_GPIO     10      /* WS2812B 数据线所接 GPIO */
#define LED_STRIP_LED_NUM  45      /* 灯珠数量 */

static led_strip_handle_t s_strip;

esp_err_t led_ctrl_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    /* RMT 配置：组件默认值即可（走线串扰才是之前混乱的根因，与 RMT 配置无关）*/
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .mem_block_symbols = 0,             /* 默认 */
        .flags = { .with_dma = 0 },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "LED strip initialized: GPIO%d, %d LEDs",
             LED_STRIP_GPIO, LED_STRIP_LED_NUM);
    return ESP_OK;
}

esp_err_t led_ctrl_set_all(uint32_t r, uint32_t g, uint32_t b)
{
    esp_err_t err = led_ctrl_init();
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < LED_STRIP_LED_NUM; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    return led_strip_refresh(s_strip);
}

/* 单颗灯珠写入缓冲（不刷新！动画应画完一帧后统一 led_ctrl_refresh，
 * 和 esp-claw 的 set_pixel + refresh 模式一致，避免逐颗刷新造成撕裂/慢）*/
esp_err_t led_ctrl_set_pixel(int index, uint32_t r, uint32_t g, uint32_t b)
{
    esp_err_t err = led_ctrl_init();
    if (err != ESP_OK) {
        return err;
    }
    if (index < 0 || index >= LED_STRIP_LED_NUM) {
        ESP_LOGW(TAG, "pixel index %d out of range (0..%d)",
                 index, LED_STRIP_LED_NUM - 1);
        return ESP_ERR_INVALID_ARG;
    }
    led_strip_set_pixel(s_strip, index, r, g, b);
    return ESP_OK;
}

/* 把缓冲里的像素一次性送到灯带 */
esp_err_t led_ctrl_refresh(void)
{
    esp_err_t err = led_ctrl_init();
    if (err != ESP_OK) {
        return err;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_ctrl_clear(void)
{
    esp_err_t err = led_ctrl_init();
    if (err != ESP_OK) {
        return err;
    }
    return led_strip_clear(s_strip);
}

int led_ctrl_get_led_count(void)
{
    return LED_STRIP_LED_NUM;
}
