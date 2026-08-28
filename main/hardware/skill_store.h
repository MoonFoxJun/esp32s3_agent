#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 技能存储（FATFS 文件系统，挂在 /storage）：
 *   每个技能 = /storage/skills/<名字>/ 目录
 *     ├─ SKILL.md     技能说明（name + description，供 skill_list 展示）
 *     └─ script.lua   Lua 脚本本体
 * 技能由 LLM 通过 skill_save 保存，之后可随时 skill_run 调用。 */

/* 挂载 FATFS 并创建 skills 目录（开机调用一次；失败不影响系统运行）*/
esp_err_t skill_store_init(void);

bool skill_store_ready(void);

/* 保存技能（name 只能含字母/数字/下划线，最长 24 字符）*/
esp_err_t skill_store_save(const char *name, const char *description, const char *script);

/* 读取技能脚本到 buf（含终止符）*/
esp_err_t skill_store_load_script(const char *name, char *buf, size_t sz);

/* 列出所有技能："- 名字: 描述\n" 格式 */
esp_err_t skill_store_list(char *out, size_t out_sz);

/* 删除技能 */
esp_err_t skill_store_delete(const char *name);
