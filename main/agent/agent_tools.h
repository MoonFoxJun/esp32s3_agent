#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 一个可被 LLM 调用的工具（function calling）
 */
typedef struct {
    const char *name;            /* 工具名，必须与 tools JSON 里的 name 一致 */
    const char *description;     /* 给 LLM 看的说明，决定它何时调用 */
    const char *parameters_json; /* 参数 JSON Schema（{"type":"object","properties":{...}}）*/
    esp_err_t (*handler)(const char *args_json, char *out, size_t out_sz);
                                 /* 执行函数：args_json 为 LLM 填的参数 JSON，
                                  * 结果写入 out（字符串形式返回给 LLM）*/
} agent_tool_t;

/**
 * 注册一个工具（重复注册同名工具会失败）
 */
esp_err_t agent_tools_register(const agent_tool_t *tool);

/**
 * 获取 "tools" 数组 JSON（内部缓存；无工具时返回 NULL）
 * 供 llm_client 构建请求体使用
 */
const char *agent_tools_get_json(void);

/**
 * 按名字执行工具，结果写入 out
 * @return ESP_OK 执行成功（结果在 out）
 */
esp_err_t agent_tools_execute(const char *name, const char *args_json,
                              char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
