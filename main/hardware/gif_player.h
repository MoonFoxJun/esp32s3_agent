#pragma once

#include "esp_err.h"

/* GIF 动画播放器（作为 display_mgr 的一个应用）：
 *   - 帧数据存在 Flash 的 "gif" 数据分区（由 tools/gif2rgb565.py 生成）
 *   - gif_player_init()    读取分区并准备帧缓冲（开机调用一次）
 *   - gif_player_register() 创建播放任务并注册到 display_mgr（开机调用一次）
 *     播放与否由 display_mgr_show(DISPLAY_APP_GIF) 决定 */

esp_err_t gif_player_init(void);
void gif_player_register(void);
