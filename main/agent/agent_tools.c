#include "agent_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "agent_tools";

#define AGENT_TOOLS_MAX 16   /* 工具上限：设备/灯/Lua/OTA/屏幕/人设 + 技能×4 */

static const agent_tool_t *s_tools[AGENT_TOOLS_MAX];
static int s_tool_count;
static char *s_tools_json_cache;   /* 懒生成的 tools 数组 JSON */

esp_err_t agent_tools_register(const agent_tool_t *tool)
{
    if (!tool || !tool->name || !tool->handler || s_tool_count >= AGENT_TOOLS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i]->name, tool->name) == 0) {
            ESP_LOGW(TAG, "Tool '%s' already registered", tool->name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_tools[s_tool_count++] = tool;

    /* 注册后缓存失效，下次 get_json 重新生成 */
    free(s_tools_json_cache);
    s_tools_json_cache = NULL;
    ESP_LOGI(TAG, "Tool registered: %s", tool->name);
    return ESP_OK;
}

const char *agent_tools_get_json(void)
{
    if (s_tool_count == 0) {
        return NULL;
    }
    if (s_tools_json_cache) {
        return s_tools_json_cache;
    }

    /* 估算大小并拼装 [{"type":"function","function":{...}},...] */
    int total = 64;
    for (int i = 0; i < s_tool_count; i++) {
        total += 128
               + (int)strlen(s_tools[i]->name)
               + (int)strlen(s_tools[i]->description)
               + (int)strlen(s_tools[i]->parameters_json);
    }
    char *buf = malloc(total);
    if (!buf) {
        return NULL;
    }

    int pos = 0;
    pos += snprintf(buf + pos, total - pos, "[");
    for (int i = 0; i < s_tool_count; i++) {
        const agent_tool_t *t = s_tools[i];
        pos += snprintf(buf + pos, total - pos,
                        "%s{\"type\":\"function\",\"function\":{"
                        "\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}}",
                        i ? "," : "",
                        t->name, t->description, t->parameters_json);
    }
    pos += snprintf(buf + pos, total - pos, "]");

    s_tools_json_cache = buf;
    return s_tools_json_cache;
}

esp_err_t agent_tools_execute(const char *name, const char *args_json,
                              char *out, size_t out_sz)
{
    if (!name || !out || out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i]->name, name) == 0) {
            return s_tools[i]->handler(args_json, out, out_sz);
        }
    }
    snprintf(out, out_sz, "unknown tool: %s", name);
    return ESP_ERR_NOT_FOUND;
}
