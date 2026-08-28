#include "skill_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "agent_tools.h"
#include "lua_engine.h"
#include "skill_store.h"

#define SKILL_SCRIPT_MAX  4096   /* 单个技能脚本上限 */
#define SKILL_RUN_TIMEOUT_MS 15000

/* 工具：skill_save(skill_name, description, script) */
static esp_err_t tool_skill_save(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name = cJSON_GetObjectItem(root, "skill_name");
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    cJSON *script = cJSON_GetObjectItem(root, "script");
    if (!cJSON_IsString(name) || !cJSON_IsString(script) ||
        strlen(script->valuestring) == 0) {
        snprintf(out, out_sz, "need 'skill_name' and 'script' strings");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = skill_store_save(name->valuestring,
                                     cJSON_IsString(desc) ? desc->valuestring : "",
                                     script->valuestring);
    cJSON_Delete(root);
    snprintf(out, out_sz, err == ESP_OK
             ? "技能已保存，以后可直接用 skill_run 调用"
             : "保存失败: %s（名字只能字母数字下划线）", esp_err_to_name(err));
    return err;
}

/* 工具：skill_run(skill_name[, mode]) */
static esp_err_t tool_skill_run(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name = cJSON_GetObjectItem(root, "skill_name");
    if (!cJSON_IsString(name)) {
        snprintf(out, out_sz, "need 'skill_name' string");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    bool bg = mode && cJSON_IsString(mode) && strcmp(mode->valuestring, "bg") == 0;

    char *script = malloc(SKILL_SCRIPT_MAX);
    if (!script) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = skill_store_load_script(name->valuestring, script, SKILL_SCRIPT_MAX);
    if (err != ESP_OK) {
        snprintf(out, out_sz, "技能不存在或读取失败: %s", esp_err_to_name(err));
        free(script);
        cJSON_Delete(root);
        return err;
    }

    if (bg) {
        /* 技能名即后台效果名（lua_effect_status 可查）*/
        bool replaced = lua_engine_bg_running();
        const char *old_name = lua_engine_bg_name();
        err = lua_engine_start_bg(name->valuestring, script);
        if (err == ESP_OK) {
            snprintf(out, out_sz, replaced
                     ? "已停止旧效果%s，技能[%s]后台运行中"
                     : "技能[%s]已后台运行，用户叫停时用 lua_stop_effect",
                     old_name ? old_name : "", name->valuestring);
        } else {
            snprintf(out, out_sz, "后台启动失败: %s", esp_err_to_name(err));
        }
    } else {
        err = lua_engine_run(script, out, out_sz, SKILL_RUN_TIMEOUT_MS);
    }
    free(script);
    cJSON_Delete(root);
    return err;
}

/* 工具：skill_list() */
static esp_err_t tool_skill_list(const char *args_json, char *out, size_t out_sz)
{
    (void)args_json;
    esp_err_t err = skill_store_list(out, out_sz);
    if (err != ESP_OK || out[0] == '\0') {
        snprintf(out, out_sz, "暂无技能。可以用 skill_save 把常用效果保存成技能。");
    }
    return ESP_OK;
}

/* 工具：skill_delete(skill_name) */
static esp_err_t tool_skill_delete(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *name = cJSON_GetObjectItem(root, "skill_name");
    if (!cJSON_IsString(name)) {
        snprintf(out, out_sz, "need 'skill_name' string");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = skill_store_delete(name->valuestring);
    cJSON_Delete(root);
    snprintf(out, out_sz, err == ESP_OK ? "技能已删除" : "删除失败: %s",
             esp_err_to_name(err));
    return err;
}

static const agent_tool_t s_tool_skill_save = {
    .name = "skill_save",
    .description = "把一段 Lua 脚本保存为可复用技能，之后用户再要同类效果时"
                   "直接 skill_run 调用，不用重新写。脚本语法与 lua_run_script 相同"
                   "（led.set/led.pixel/sleep）。用户说'记住这个效果/存成技能'时使用。",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"skill_name\":{\"type\":\"string\",\"description\":\"技能名（字母数字下划线）\"},"
        "\"description\":{\"type\":\"string\",\"description\":\"技能效果说明，供以后选择\"},"
        "\"script\":{\"type\":\"string\",\"description\":\"Lua 脚本\"}"
        "},"
        "\"required\":[\"skill_name\",\"description\",\"script\"]"
        "}",
    .handler = tool_skill_save,
};

static const agent_tool_t s_tool_skill_run = {
    .name = "skill_run",
    .description = "运行已保存的技能（skill_save 存的）。mode='bg' 后台持续运行"
                   "（用户叫停用 lua_stop_effect），否则一次性执行。",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"skill_name\":{\"type\":\"string\",\"description\":\"技能名\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"once\",\"bg\"],"
        "\"description\":\"once=一次(默认), bg=后台持续\"}"
        "},"
        "\"required\":[\"skill_name\"]"
        "}",
    .handler = tool_skill_run,
};

static const agent_tool_t s_tool_skill_list = {
    .name = "skill_list",
    .description = "列出所有已保存的技能及其说明。用户提到某个技能、或想确认"
                   "有没有现成效果可用时调用。",
    .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
    .handler = tool_skill_list,
};

static const agent_tool_t s_tool_skill_delete = {
    .name = "skill_delete",
    .description = "删除一个已保存的技能。用户要求删除/移除某个技能时调用。",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"skill_name\":{\"type\":\"string\",\"description\":\"要删除的技能名\"}"
        "},"
        "\"required\":[\"skill_name\"]"
        "}",
    .handler = tool_skill_delete,
};

esp_err_t skill_tool_register(void)
{
    esp_err_t err = agent_tools_register(&s_tool_skill_save);
    if (err != ESP_OK) {
        return err;
    }
    err = agent_tools_register(&s_tool_skill_run);
    if (err != ESP_OK) {
        return err;
    }
    err = agent_tools_register(&s_tool_skill_list);
    if (err != ESP_OK) {
        return err;
    }
    return agent_tools_register(&s_tool_skill_delete);
}
