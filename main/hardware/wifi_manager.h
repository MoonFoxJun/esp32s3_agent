#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 NVS 存储 */
esp_err_t wifi_manager_init_nvs(void);

/* 从 NVS 加载保存的 WiFi 配置 */
bool wifi_manager_load_config(char *ssid, size_t ssid_size,
                               char *password, size_t password_size);

/* 将 WiFi 配置保存到 NVS */
void wifi_manager_save_config(const char *ssid, const char *password);

/* 初始化 WiFi 并连接，返回事件组用于等待连接完成 */
EventGroupHandle_t wifi_manager_init(const char *ssid, const char *password);

/* WiFi 连接状态位 */
#define WIFI_MANAGER_CONNECTED_BIT  BIT0

#ifdef __cplusplus
}
#endif