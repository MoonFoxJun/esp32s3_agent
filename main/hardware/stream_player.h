#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 串口串流播放器（作为 display_mgr 的一个应用）：
 *   - PC 端 tools/screen_stream.py 截屏 → JPEG 压缩 → 串口发送
 *   - ESP32 用 TJpgDec 解压 → RGB565 → 全屏显示
 *   - 协议：连续的独立 JPEG 帧（无帧头，TJpgDec 自己识别帧边界）
 *   - 停止：PC 停止发送约 2 秒后自动退出串流，回到 GIF */

/* 初始化：分配帧缓冲并注册任务（开机调用一次，任务挂起等待被激活）*/
esp_err_t stream_player_init(void);

/* 当前是否正在串流（供工具/命令判断，避免串流中切走屏幕）*/
bool stream_player_active(void);
