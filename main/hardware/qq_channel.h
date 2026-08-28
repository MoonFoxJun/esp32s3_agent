#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 QQ 机器人通道（凭证在 qq_channel.c 顶部配置）
 *
 * 工作方式：
 *  - 换取 access_token → 获取 WebSocket 网关 → 连接 + identify + 心跳
 *  - 收到私聊/群@消息 → 转 agent_msg_t 投递到 agent 队列（CH_QQ）
 *  - session_id 约定："c2c_<openid>"（私聊）/ "grp_<openid>"（群）
 */
esp_err_t qq_channel_start(void);

/**
 * 回发文本（由 agent_core 按 channel 分发调用）
 * @param chat_id 带前缀的会话 id（c2c_xxx / grp_xxx）
 * @param text    回复内容
 */
esp_err_t qq_channel_send_text(const char *chat_id, const char *text);

#ifdef __cplusplus
}
#endif
