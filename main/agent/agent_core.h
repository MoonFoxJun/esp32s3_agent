#pragma once

#include "esp_err.h"
#include "agent_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 Agent 核心任务（消息队列的唯一消费者）
 *
 * - 创建共享输入队列，通道侧用 agent_core_submit() 投递消息
 * - 核心任务串行处理：取消息 → 分类 → 调 LLM/命令 → 按 channel 回发
 * - 全局唯一调用 llm_client_chat() 的地方（解决多通道并发线程安全问题）
 *
 * @return ESP_OK 成功
 */
esp_err_t agent_core_start(void);

/**
 * 通道侧投递消息入口（生产者调用，非阻塞）
 *
 * @param msg 统一消息（调用后即复制进队列，可复用栈上的 msg）
 */
void agent_core_submit(const agent_msg_t *msg);

#ifdef __cplusplus
}
#endif
