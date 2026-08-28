#include "led_tool.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "agent_tools.h"
#include "led_ctrl.h"

/* 工具：led_set(state, color, brightness)
 * 例：{"state":"on","color":"red","brightness":128} */
static esp_err_t tool_led_set(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* 默认 off */
    cJSON *state = cJSON_GetObjectItem(root, "state");
    bool on = cJSON_IsString(state) && strcmp(state->valuestring, "on") == 0;
    if (!on) {
        esp_err_t err = led_ctrl_clear();
        snprintf(out, out_sz, err == ESP_OK ? "LED off" : "LED clear failed: %s",
                 err == ESP_OK ? "" : esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* 颜色，默认白 */
    uint32_t r = 255, g = 255, b = 255;
    cJSON *color = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsString(color)) {
        if (strcmp(color->valuestring, "red") == 0) {
            r = 255; g = 0;   b = 0;
        } else if (strcmp(color->valuestring, "green") == 0) {
            r = 0;   g = 255; b = 0;
        } else if (strcmp(color->valuestring, "blue") == 0) {
            r = 0;   g = 0;   b = 255;
        } else if (strcmp(color->valuestring, "white") == 0) {
            r = 255; g = 255; b = 255;
        } else {
            snprintf(out, out_sz, "unknown color: %s (red/green/blue/white)",
                     color->valuestring);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* 亮度 0-255，缩放 RGB */
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        int br = brightness->valueint;
        if (br < 0) br = 0;
        if (br > 255) br = 255;
        r = (uint32_t)r * br / 255;
        g = (uint32_t)g * br / 255;
        b = (uint32_t)b * br / 255;
    }

    esp_err_t err = led_ctrl_set_all(r, g, b);
    if (err == ESP_OK) {
        snprintf(out, out_sz, "LED on (r=%lu,g=%lu,b=%lu, %d leds)",
                 (unsigned long)r, (unsigned long)g, (unsigned long)b,
                 led_ctrl_get_led_count());
    } else {
        snprintf(out, out_sz, "LED set failed: %s", esp_err_to_name(err));
    }
    cJSON_Delete(root);
    return err;
}

static const agent_tool_t s_tool_led = {
    .name = "led_set",
    .description = "控制 WS2812B 灯条：开关、颜色(red/green/blue/white)、亮度(0-255)",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"],\"description\":\"开关状态\"},"
        "\"color\":{\"type\":\"string\",\"enum\":[\"red\",\"green\",\"blue\",\"white\"],\"description\":\"颜色\"},"
        "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"亮度0-255\"}"
        "},"
        "\"required\":[\"state\"]"
        "}",
    .handler = tool_led_set,
};

esp_err_t led_tool_register(void)
{
    return agent_tools_register(&s_tool_led);
}
