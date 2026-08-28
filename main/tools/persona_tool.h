#pragma once

#include "esp_err.h"

/* 注册 LLM 工具 set_persona(persona)：修改助手人设/性格。
 * 新设定会持久化到 NVS，重启后依然生效。 */
esp_err_t persona_tool_register(void);
