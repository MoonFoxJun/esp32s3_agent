#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 注册 lua_run_script 工具：让 LLM 写 Lua 脚本控制硬件
 */
esp_err_t lua_tool_register(void);

#ifdef __cplusplus
}
#endif
