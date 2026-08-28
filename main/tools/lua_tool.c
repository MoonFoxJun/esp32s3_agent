#include "lua_tool.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "agent_tools.h"
#include "lua_engine.h"

/* 一次性脚本超时（墙钟时间，含 sleep）。持续效果请用 mode="bg"。*/
#define LUA_TOOL_TIMEOUT_MS  15000

/* 工具：lua_run_script(script[, mode[, effect_name]])
 * mode: "once"（默认）一次性执行；"bg" 后台持续运行（直到 lua_stop_effect）
 * 例：{"script":"while true do ... end","mode":"bg","effect_name":"flow"} */
static esp_err_t tool_lua_run(const char *args_json, char *out, size_t out_sz)
{
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        snprintf(out, out_sz, "bad arguments JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *script = cJSON_GetObjectItem(root, "script");
    if (!cJSON_IsString(script) || strlen(script->valuestring) == 0) {
        snprintf(out, out_sz, "need 'script' string argument");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    bool bg = mode && cJSON_IsString(mode) && strcmp(mode->valuestring, "bg") == 0;

    esp_err_t err;
    if (bg) {
        /* 先查旧效果：启动新效果会替换旧效果，必须明确告知 LLM（无隐式魔法）*/
        bool replaced = lua_engine_bg_running();
        const char *old_name = lua_engine_bg_name();
        cJSON *ename = cJSON_GetObjectItem(root, "effect_name");
        const char *effect_name = (cJSON_IsString(ename) && ename->valuestring[0])
                                  ? ename->valuestring : "effect";

        err = lua_engine_start_bg(effect_name, script->valuestring);
        if (err == ESP_OK) {
            if (replaced) {
                snprintf(out, out_sz,
                         "已自动停止旧效果%s，新效果[%s]后台运行中；"
                         "可用 lua_effect_status 查看、lua_stop_effect 停止",
                         old_name ? old_name : "", effect_name);
            } else {
                snprintf(out, out_sz,
                         "后台效果[%s]已启动；可用 lua_effect_status 查看、"
                         "lua_stop_effect 停止", effect_name);
            }
        } else {
            snprintf(out, out_sz, "后台启动失败: %s", esp_err_to_name(err));
        }
    } else {
        err = lua_engine_run(script->valuestring, out, out_sz, LUA_TOOL_TIMEOUT_MS);
    }
    cJSON_Delete(root);
    return err;
}

/* 工具：lua_effect_status() —— 查询当前后台效果 */
static esp_err_t tool_lua_status(const char *args_json, char *out, size_t out_sz)
{
    (void)args_json;
    if (lua_engine_bg_running()) {
        const char *name = lua_engine_bg_name();
        snprintf(out, out_sz, "正在后台运行的效果: %s", name ? name : "(未命名)");
    } else {
        snprintf(out, out_sz, "当前没有后台效果在运行");
    }
    return ESP_OK;
}

/* 工具：lua_stop_effect() —— 停止后台持续效果 */
static esp_err_t tool_lua_stop(const char *args_json, char *out, size_t out_sz)
{
    (void)args_json;
    if (lua_engine_bg_running()) {
        const char *name = lua_engine_bg_name();
        lua_engine_stop_bg();
        snprintf(out, out_sz, "已停止后台效果%s", name ? name : "");
    } else {
        snprintf(out, out_sz, "当前没有正在运行的后台效果");
    }
    return ESP_OK;
}

static const agent_tool_t s_tool_lua = {
    .name = "lua_run_script",
    .description = "执行一段 Lua 脚本控制 GPIO10 上的 WS2812B 灯带（45 颗灯珠）。"
                   "LED API：led.set(state,color,brightness) 整条灯带变色并立即显示"
                   "(color: red/orange/yellow/green/cyan/blue/purple/white/off, 0-255)；"
                   "led.pixel(index,color)/led.rgb(index,r,g,b)/led.hsv(index,h,s,v) "
                   "只写入缓冲不显示（hsv: h=0-360色相, s/v=0-255，渐变效果用）——"
                   "画完一帧后必须调 led.show() 一次性送到灯带；"
                   "sleep(ms) 延时，print(...) 输出。"
                   "mode：'once'（默认）一次执行最长 15 秒；"
                   "'bg' 后台持续运行直到用户叫停（可给 effect_name 命名）。"
                   "抢占规则：启动新后台效果会自动停止旧效果，结果里会明确告知；"
                   "lua_effect_status 可查询当前效果，lua_stop_effect 可停止。"
                   "渐变彩虹模板（45颗横跨色相环缓慢旋转，画完一帧 led.show 一次）："
                   "while true do for offset=0,359,3 do for i=1,45 do "
                   "led.hsv(i,(offset+(i-1)*8)%360,255,255) end "
                   "led.show() sleep(25) end end",
    .parameters_json = "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "\"script\":{\"type\":\"string\",\"description\":\"要执行的 Lua 代码\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"once\",\"bg\"],"
        "\"description\":\"once=一次执行(默认), bg=后台持续运行\"},"
        "\"effect_name\":{\"type\":\"string\",\"description\":\"后台效果名（mode=bg 时可选）\"}"
        "},"
        "\"required\":[\"script\"]"
        "}",
    .handler = tool_lua_run,
};

static const agent_tool_t s_tool_lua_status = {
    .name = "lua_effect_status",
    .description = "查询当前是否有后台 Lua 效果在运行、叫什么名字。"
                   "用户问'现在有什么效果在跑/灯还在闪吗/那个效果叫什么'时调用。",
    .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
    .handler = tool_lua_status,
};

static const agent_tool_t s_tool_lua_stop = {
    .name = "lua_stop_effect",
    .description = "停止正在后台运行的 Lua 持续效果（流水灯/呼吸/彩虹等）。"
                   "用户要求停止灯光效果时调用。",
    .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
    .handler = tool_lua_stop,
};

esp_err_t lua_tool_register(void)
{
    esp_err_t err = agent_tools_register(&s_tool_lua);
    if (err != ESP_OK) {
        return err;
    }
    err = agent_tools_register(&s_tool_lua_status);
    if (err != ESP_OK) {
        return err;
    }
    return agent_tools_register(&s_tool_lua_stop);
}
