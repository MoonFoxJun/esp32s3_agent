#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLM_MAX_HISTORY  8   /* 最多保存的历史消息数 */

/**
 * 初始化 LLM 客户端
 *
 * @param base_url  LLM API 地址，如 "https://api.openai.com/v1"
 * @param api_key   API 密钥
 * @param model     模型名称，如 "gpt-3.5-turbo"
 * @return ESP_OK 成功
 */
esp_err_t llm_client_init(const char *base_url, const char *api_key, const char *model);

/**
 * 设置/更新 System Prompt（给 LLM 设定人设）
 *
 * @param prompt 系统提示词 (NULL 则清除)
 * @return ESP_OK 成功
 */
esp_err_t llm_client_set_system_prompt(const char *prompt);

/**
 * 是否从 NVS 恢复过已保存的人设。
 * 开机时用它判断：有存档用存档（用户可随时改人设并持久化），
 * 没有才用 main.c 里的默认人设。
 */
bool llm_client_has_saved_prompt(void);

/**
 * 发送一条消息给 LLM，获取回复（非流式，同步阻塞）
 * 自动将对话加入历史，下次请求会携带完整上下文
 *
 * @param user_message  用户输入的文本
 * @param reply         输出缓冲区（存放 LLM 回复）
 * @param reply_size    输出缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t llm_client_chat(const char *user_message, char *reply, size_t reply_size);

/**
 * 清除对话历史（不清除 System Prompt）
 */
void llm_client_clear_history(void);

/**
 * 释放 LLM 客户端资源
 */
void llm_client_deinit(void);

#ifdef __cplusplus
}
#endif