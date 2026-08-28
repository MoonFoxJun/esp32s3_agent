#pragma once

#include "esp_err.h"

/* 注册 LLM 工具 screen_switch(mode)：切换 TFT 显示内容
 * mode: "gif" 动画 | "idle" 黑屏 | "stream" 串流 */
esp_err_t screen_tool_register(void);
