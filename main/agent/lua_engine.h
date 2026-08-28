#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 Lua 运行时（幂等）
 *
 * 只加载安全库（base/string/table/math/utf8，不含 io/os/debug——LLM 代码
 * 碰不到文件系统和 shell），并注册硬件绑定：
 *   led.set(state, color, brightness)   控制 WS2812B 灯条
 *   sleep(ms)                           延时
 *   print(...)                          输出到结果缓冲
 */
esp_err_t lua_engine_init(void);

/**
 * 执行一段 Lua 脚本
 *
 * @param script     脚本源码（LLM 生成的代码）
 * @param out        输出缓冲（print 的输出 + 错误信息）
 * @param out_sz     输出缓冲大小
 * @param timeout_ms 超时（毫秒），0 表示不设超时；超时由指令计数钩子触发
 * @return ESP_OK 执行成功；ESP_FAIL 脚本出错（错误信息在 out）
 */
esp_err_t lua_engine_run(const char *script, char *out, size_t out_sz, uint32_t timeout_ms);

/**
 * 后台脚本（持续效果）：
 *   start_bg 在独立任务里跑脚本，脚本可以 while true 无限循环
 *   （流水灯/呼吸/彩虹这类"一直跑直到叫停"的效果）；
 *   stop_bg 协作式停止（脚本钩子在下一次指令计数点中止）。
 *   同一时刻只允许一个后台脚本；启动新效果会自动停止旧效果
 *   （工具层会把"替换了旧效果"明确告知 LLM，无隐式优先级）。
 *   name 是效果名（工具层查询/汇报用，可传 NULL 表示匿名）。
 */
esp_err_t lua_engine_start_bg(const char *name, const char *script);
void lua_engine_stop_bg(void);
bool lua_engine_bg_running(void);
const char *lua_engine_bg_name(void);   /* 当前后台效果名（无则 NULL）*/

#ifdef __cplusplus
}
#endif
