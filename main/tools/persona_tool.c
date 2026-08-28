#include "persona_tool.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "agent_tools.h"
#include "llm_client.h"

/* 工具：set_persona(persona)
 * 例：{"persona":"你叫小蓝，说话俏皮，喜欢用表情，回答控制在30字内"} */
static esp_err_t tool_set_persona(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *persona = cJSON_GetObjectItem(root, "persona");
    if (!cJSON_IsString(persona) || strlen(persona->valuestring) == 0) {
        snprintf(out, out_sz, "need 'persona' string argument");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = llm_client_set_system_prompt(persona->valuestring);
    cJSON_Delete(root);
    snprintf(out, out_sz, err == ESP_OK
             ? "人设已更新并保存（断电不丢），从现在起按新人设回答"
             : "人设更新失败: %s", esp_err_to_name(err));
    return err;
}

static const agent_tool_t s_tool_persona = {
    .name = "set_persona",
    .description = "修改助手的人设/性格/行为规则。当用户要求你改变说话风格、名字、"
                   "身份、语气、行为准则时调用。新设定会自动保存，重启后依然生效。"
                   "示例 persona：'你叫小蓝，说话俏皮，喜欢用表情'。",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"persona\":{\"type\":\"string\",\"description\":\"新的人设描述\"}"
        "},"
        "\"required\":[\"persona\"]"
        "}",
    .handler = tool_set_persona,
};

esp_err_t persona_tool_register(void)
{
    return agent_tools_register(&s_tool_persona);
}
