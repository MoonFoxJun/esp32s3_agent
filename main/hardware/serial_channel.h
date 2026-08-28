#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动串口通道任务（生产者）
 *
 * 循环 readline → 本地命令（screen/stream）或
 * 组装 agent_msg_t{CH_SERIAL, "uart"} → agent_core_submit()
 * 只负责"收 + 投递"，不接触 LLM。
 *
 * @return ESP_OK 成功
 */
esp_err_t serial_channel_start(void);

/**
 * 挂起/恢复串口通道任务。
 * 串流模式下串口被二进制数据占用，控制台行读取必须暂停；
 * 串流结束后恢复。由 stream_player 调用。
 */
void serial_channel_suspend(void);
void serial_channel_resume(void);

#ifdef __cplusplus
}
#endif
