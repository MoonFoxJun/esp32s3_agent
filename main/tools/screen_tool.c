#include "screen_tool.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "agent_tools.h"
#include "display_mgr.h"
#include "screen.h"
#include "stream_player.h"

/* 工具：screen_switch(mode)
 * 例：{"mode":"gif"} 切到 GIF 动画；{"mode":"idle"} 黑屏；{"mode":"stream"} 串流 */
static esp_err_t tool_screen_switch(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsString(mode)) {
        snprintf(out, out_sz, "need 'mode' string: gif / idle / stream");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(mode->valuestring, "gif") == 0) {
        if (stream_player_active()) {
            snprintf(out, out_sz, "当前正在串流，请先让电脑端停止发送画面");
        } else {
            display_mgr_show(DISPLAY_APP_GIF);
            snprintf(out, out_sz, "屏幕已切换到 GIF 动画");
        }
    } else if (strcmp(mode->valuestring, "idle") == 0) {
        if (stream_player_active()) {
            snprintf(out, out_sz, "当前正在串流，请先让电脑端停止发送画面");
        } else {
            display_mgr_show(DISPLAY_APP_NONE);
            screen_fill(0x0000);
            snprintf(out, out_sz, "屏幕已关闭（黑屏）");
        }
    } else if (strcmp(mode->valuestring, "stream") == 0) {
        display_mgr_show(DISPLAY_APP_STREAM);
        snprintf(out, out_sz, "已切换串流模式，等待电脑端发送画面");
    } else {
        snprintf(out, out_sz, "mode 必须是 gif / idle / stream 之一");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static const agent_tool_t s_tool_screen = {
    .name = "screen_switch",
    .description = "切换 TFT 屏幕显示内容。mode 为 'gif' 播放 GIF 动画，"
                   "'idle' 黑屏，'stream' 进入串流模式（等电脑端发画面）。"
                   "用户要求显示/隐藏动画或屏幕时使用。",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"gif\",\"idle\",\"stream\"],"
        "\"description\":\"gif=动画, idle=黑屏, stream=串流\"}"
        "},"
        "\"required\":[\"mode\"]"
        "}",
    .handler = tool_screen_switch,
};

esp_err_t screen_tool_register(void)
{
    return agent_tools_register(&s_tool_screen);
}
