#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "wifi_manager.h"
#include "llm_client.h"
#include "serial_console.h"
#include "agent_core.h"
#include "serial_channel.h"
#include "agent_tools.h"
#include "led_tool.h"
#include "lua_engine.h"
#include "lua_tool.h"
#include "ota_tool.h"
#include "qq_channel.h"
#include "screen.h"
#include "gif_player.h"
#include "display_mgr.h"
#include "stream_player.h"
#include "screen_tool.h"
#include "persona_tool.h"
#include "skill_store.h"
#include "skill_tool.h"

static const char *TAG = "sample_agent";

/* ========== 阶段2: LLM 通信 ========== */

/* LLM 配置信息
 * 发布前请替换为自己的 Key！API Key 可申请自：
 * - DeepSeek (platform.deepseek.com)
 * - 阿里云通义千问 (dashscope.aliyuncs.com)
 * - OpenAI (openai.com)
 */
#define LLM_BASE_URL  "https://api.deepseek.com/v1"   // 替换为你的 API 地址
#define LLM_API_KEY   "YOUR_DEEPSEEK_API_KEY"         // 替换为你的 API Key（勿公开！）
#define LLM_MODEL     "deepseek-chat"                 // 替换为你的模型名称

/* ========== 阶段4.2: LLM 工具定义 ========== */

/* 按"栈余量从少到多"排序：最危险的排最前面 */
static int cmp_by_free_stack(const void *a, const void *b)
{
    const TaskStatus_t *ta = (const TaskStatus_t *)a;
    const TaskStatus_t *tb = (const TaskStatus_t *)b;
    return (int)ta->usStackHighWaterMark - (int)tb->usStackHighWaterMark;
}

/* 内存体检工具：堆 + 每个任务的栈余量（余量 < 1KB 标 !! 危险）*/
static esp_err_t tool_device_info(const char *args_json, char *out, size_t out_sz)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int pos = snprintf(out, out_sz,
                       "ESP32-S3, ESP-IDF %s\n"
                       "堆: 内部 %u KB 可用, PSRAM %u KB 可用\n"
                       "任务栈余量(危险 < 1024B; ipc0/ipc1/IDLE 系统任务余量低属正常):\n",
                       IDF_VER,
                       (unsigned)(free_internal / 1024), (unsigned)(free_psram / 1024));

    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(sizeof(TaskStatus_t) * count);
    if (!tasks) {
        return ESP_OK;
    }
    count = uxTaskGetSystemState(tasks, count, NULL);
    qsort(tasks, count, sizeof(TaskStatus_t), cmp_by_free_stack);

    for (UBaseType_t i = 0; i < count && pos < (int)out_sz - 64; i++) {
        int free_b = (int)tasks[i].usStackHighWaterMark;
        const char *mark = (free_b < 1024) ? " !!" : "";
        pos += snprintf(out + pos, out_sz - pos, "  %-16s %5d B%s\n",
                        tasks[i].pcTaskName, free_b, mark);
    }
    free(tasks);
    return ESP_OK;
}

static const agent_tool_t s_tool_device_info = {
    .name = "device_info",
    .description = "设备内存体检：芯片型号、ESP-IDF 版本、内部堆/PSRAM 剩余、"
                   "每个任务的栈余量（余量<1KB 标记危险）。用户询问设备状态、"
                   "内存/卡顿时调用。",
    .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
    .handler = tool_device_info,
};

void app_main(void)
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "=== Sample Agent Starting ===");

    /* 1. 初始化 NVS (持久化存储) */
    ESP_ERROR_CHECK(wifi_manager_init_nvs());

    /* 2. 尝试加载保存的 WiFi 配置 */
    char ssid[33] = {0};
    char password[65] = {0};
    bool has_wifi_config = wifi_manager_load_config(ssid, sizeof(ssid),
                                                     password, sizeof(password));

    /* 3. 如果没有保存的配置，使用默认配置（请改成自己的 WiFi）*/
    if (!has_wifi_config) {
        const char *default_ssid = "YOUR_WIFI_SSID";
        const char *default_password = "YOUR_WIFI_PASSWORD";
        strncpy(ssid, default_ssid, sizeof(ssid) - 1);
        strncpy(password, default_password, sizeof(password) - 1);

        /* 保存到 NVS，下次启动自动连接 */
        wifi_manager_save_config(ssid, password);
    }

    /* 4. 初始化 WiFi */
    EventGroupHandle_t wifi_group = wifi_manager_init(ssid, password);

    /* 5. 等待 WiFi 连接 (最多等待 30 秒) */
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_group, WIFI_MANAGER_CONNECTED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_MANAGER_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully!");
    } else {
        ESP_LOGW(TAG, "WiFi connection timed out, but continuing...");
    }

    /* 6. 初始化 LLM 客户端 */
    ESP_LOGI(TAG, "Initializing LLM client...");
    err = llm_client_init(LLM_BASE_URL, LLM_API_KEY, LLM_MODEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LLM client: %s", esp_err_to_name(err));
    }

    /* 6.1 设置 System Prompt（给 LLM 设定行为/人设）
     *     来源：NVS 里有用户保存过的（set_persona 工具改的）就用存档，
     *     否则用默认人设。存档优先级高，改过就永久生效、断电不丢。 */
    if (!llm_client_has_saved_prompt()) {
        llm_client_set_system_prompt(
            "你是一个运行在 ESP32 微型设备上的智能助手。"
            "请保持回答简洁（50字以内），因为设备显示和存储能力有限。"
            "如果用户没有指定语言，请用中文回答。"
        );
    } else {
        ESP_LOGI(TAG, "Using saved persona from NVS");
    }

    /* 6.2 注册 LLM 可调用工具（Phase 4.2/4.3/4.5/6）*/
    agent_tools_register(&s_tool_device_info);
    led_tool_register();
    lua_engine_init();       /* Lua 运行时（LLM 写脚本执行）*/
    lua_tool_register();     /* lua_run_script / lua_stop_effect 工具 */
    ota_tool_register();     /* ota_update 固件升级工具 */
    screen_tool_register();  /* screen_switch 工具（LLM 切屏）*/
    persona_tool_register(); /* set_persona 工具（LLM 改人设）*/
    skill_tool_register();   /* skill_save/run/list/delete（技能包）*/

    /* 7. 初始化串口控制台 */
    ESP_LOGI(TAG, "Initializing serial console...");
    serial_console_init(NULL);
    serial_console_printf("\n=== ESP32 Agent Serial Console ===\n");
    serial_console_printf("Type a message and press Enter to chat with LLM\n");
    serial_console_printf("Commands: clear (clear history)\n");

    /* 7.1 初始化 ST7789 屏幕（颜色通道已调通，无需闪烁自检）*/
    screen_init();

    /* 7.2 挂载技能存储（FATFS，/storage；失败不影响主功能）*/
    skill_store_init();

    /* 7.2 屏幕显示仲裁 + 应用注册
     *     display_mgr 保证同一时刻只有一个应用占用屏幕
     *     （GIF 播放 / PC 串流），切换入口：串口命令或 LLM 工具 */
    display_mgr_init();
    if (gif_player_init() == ESP_OK) {
        gif_player_register();
    }
    stream_player_init();
    display_mgr_show(DISPLAY_APP_GIF);   /* 默认播放 GIF 动画 */

    /* 9. 启动 Agent 核心（消息队列唯一消费者）+ 串口通道（生产者） */
    err = agent_core_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start agent core: %s", esp_err_to_name(err));
        return;
    }
    err = serial_channel_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start serial channel: %s", esp_err_to_name(err));
        return;
    }

    /* 9.1 启动 QQ 机器人通道（凭证在 qq_channel.c 顶部配置）*/
    qq_channel_start();

    ESP_LOGI(TAG, "=== Sample Agent Running ===");
    /* main 只做初始化，之后把 CPU 让给各任务，自己睡觉 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
