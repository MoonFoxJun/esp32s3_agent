#include "ota_tool.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "agent_tools.h"

static const char *TAG = "ota_tool";

#define OTA_HTTP_TIMEOUT_MS  30000

/* 工具：ota_update(url)
 * 例：{"url":"https://example.com/firmware.bin"}
 * 升级成功后设备自动重启到新固件 */
static esp_err_t tool_ota_update(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url) || strlen(url->valuestring) == 0) {
        snprintf(out, out_sz, "need 'url' string argument");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(out, out_sz, "OTA starting: %s ...", url->valuestring);
    ESP_LOGI(TAG, "OTA update from %s", url->valuestring);

    esp_http_client_config_t http_cfg = {
        .url = url->valuestring,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        snprintf(out, out_sz, "OTA failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(out, out_sz, "OTA success, rebooting...");
    ESP_LOGI(TAG, "OTA success, rebooting in 1s...");
    vTaskDelay(pdMS_TO_TICKS(1000));   /* 留时间让结果发出去 */
    esp_restart();                     /* 重启到新固件 */
    return ESP_OK;                     /* 不会执行到这里 */
}

static const agent_tool_t s_tool_ota = {
    .name = "ota_update",
    .description = "固件在线升级：从指定 https 地址下载新的固件 .bin 并烧录，成功后设备自动重启。"
                   "仅在用户明确要求升级固件时调用。",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":\"固件 .bin 文件的 https 下载地址\"}"
        "},"
        "\"required\":[\"url\"]"
        "}",
    .handler = tool_ota_update,
};

esp_err_t ota_tool_register(void)
{
    return agent_tools_register(&s_tool_ota);
}
