#pragma once

#include "esp_err.h"

/* 注册技能相关 LLM 工具：
 *   skill_save(name, description, script)  把 Lua 脚本存成可复用技能
 *   skill_run(name[, mode])                运行已保存的技能
 *   skill_list()                           列出所有技能
 *   skill_delete(name)                     删除技能 */
esp_err_t skill_tool_register(void);
