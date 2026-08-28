#include "serial_channel.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "agent_core.h"
#include "display_mgr.h"
#include "lua_engine.h"
#include "screen.h"
#include "serial_console.h"
#include "skill_store.h"

static const char *TAG = "serial_channel";

/* 栈：lua 命令在这里直接跑 Lua 解释器（需要 ~8KB，曾 3072 溢出重启）*/
#define SERIAL_CHANNEL_STACK 8192

static TaskHandle_t s_task = NULL;

/* 本地命令：屏幕/GIF/串流切换。
 * 返回 true 表示已处理（不送给 LLM）*/
static bool handle_local_command(const char *line)
{
    if (strcmp(line, "screen gif") == 0) {
        display_mgr_show(DISPLAY_APP_GIF);
        serial_console_printf("display: GIF\n");
        return true;
    }
    if (strcmp(line, "screen idle") == 0) {
        display_mgr_show(DISPLAY_APP_NONE);
        screen_fill(0x0000);   /* 黑屏 */
        serial_console_printf("display: idle\n");
        return true;
    }
    if (strcmp(line, "stream on") == 0) {
        serial_console_printf("stream: 请运行 PC 端 screen_stream.py\n");
        display_mgr_show(DISPLAY_APP_STREAM);   /* 恢复串流任务，接管串口 */
        return true;
    }
    if (strcmp(line, "skill list") == 0) {
        char buf[512];
        if (skill_store_list(buf, sizeof(buf)) == ESP_OK && buf[0]) {
            serial_console_printf("技能:\n%s", buf);
        } else {
            serial_console_printf("暂无技能\n");
        }
        return true;
    }
    if (strncmp(line, "skill del ", 10) == 0) {
        const char *name = line + 10;
        esp_err_t err = skill_store_delete(name);
        serial_console_printf("skill %s: %s\n", name,
                              err == ESP_OK ? "已删除" : esp_err_to_name(err));
        return true;
    }
    if (strncmp(line, "lua ", 4) == 0) {        /* 直接执行 Lua 脚本（调试/测试用，绕过 LLM）：
         * 例: lua led.rgb(1,255,0,0) led.show()
         *     lua bg while true do ... end  ← 后台持续运行 */
        const char *script = line + 4;
        if (strncmp(script, "bg ", 3) == 0) {
            script += 3;
            esp_err_t err = lua_engine_start_bg("console", script);
            serial_console_printf("lua bg: %s\n", err == ESP_OK ? "已后台启动" : esp_err_to_name(err));
            return true;
        }
        if (script[0] == '\0') {
            serial_console_printf("用法: lua <Lua代码> 或 lua bg <Lua代码>\n");
            return true;
        }
        char out[512];
        esp_err_t err = lua_engine_run(script, out, sizeof(out), 15000);
        serial_console_printf("lua: %s\n%s\n", esp_err_to_name(err), out);
        return true;
    }
    return false;
}

static void serial_channel_task(void *arg)
{
    char input_buf[256];

    serial_console_printf("\n> ");
    while (1) {
        /* 5 秒无换行也自动提交：兼容不发换行符的终端；
         * 空超时（无输入）则安静等待，不重打提示符刷屏 */
        esp_err_t err = serial_console_readline(input_buf, sizeof(input_buf), 5000);
        if (err != ESP_OK || strlen(input_buf) == 0) {
            continue;
        }

        /* 本地命令优先：screen/stream 这类硬件控制不发给 LLM */
        if (handle_local_command(input_buf)) {
            serial_console_printf("\n> ");
            continue;
        }

        agent_msg_t msg = {
            .channel = CH_SERIAL,
        };
        strlcpy(msg.session_id, "uart", sizeof(msg.session_id));
        strlcpy(msg.content, input_buf, sizeof(msg.content));
        agent_core_submit(&msg);

        serial_console_printf("\n> ");
    }
}

esp_err_t serial_channel_start(void)
{
    BaseType_t ret = xTaskCreate(serial_channel_task, "serial_ch",
                                 SERIAL_CHANNEL_STACK, NULL, 2, &s_task);
    if (ret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serial channel started");
    return ESP_OK;
}

void serial_channel_suspend(void)
{
    if (s_task) {
        vTaskSuspend(s_task);
    }
}

void serial_channel_resume(void)
{
    if (s_task) {
        vTaskResume(s_task);
    }
}
