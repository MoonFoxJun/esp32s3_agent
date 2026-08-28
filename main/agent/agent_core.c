#include "agent_core.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "llm_client.h"
#include "qq_channel.h"
#include "serial_console.h"

static const char *TAG = "agent_core";

#define AGENT_QUEUE_LEN   8      /* 输入队列深度 */
/* 栈大小：工具（尤其 lua_run_script 的 Lua 解释器 + LED 驱动链）在
 * 本任务内执行，4096 会溢出（曾触发 stack overflow 重启）；8192 保险。*/
#define AGENT_TASK_STACK  8192
#define AGENT_REPLY_SIZE  4096

static QueueHandle_t s_agent_queue;

void agent_core_submit(const agent_msg_t *msg)
{
    if (!s_agent_queue || !msg) {
        return;
    }
    if (xQueueSend(s_agent_queue, msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Agent queue full, message dropped");
    }
}

/* 按来源通道回发文本（通道多了就往这里加 case）*/
static void reply_to_channel(const agent_msg_t *msg, const char *text)
{
    if (!msg || !text) {
        return;
    }
    switch (msg->channel) {
    case CH_SERIAL:
        serial_console_printf("%s\n", text);
        break;
    case CH_QQ:
        qq_channel_send_text(msg->session_id, text);
        break;
    default:
        ESP_LOGW(TAG, "No reply path for channel=%d", (int)msg->channel);
        break;
    }
}

static void agent_core_task(void *arg)
{
    agent_msg_t msg;

    while (1) {
        /* 睡在队列上：没有消息不占 CPU */
        if (xQueueReceive(s_agent_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 命令层：clear 清历史；exit 在多通道模式下不再退出程序 */
        if (strcmp(msg.content, "clear") == 0) {
            llm_client_clear_history();
            reply_to_channel(&msg, "Conversation history cleared.");
            continue;
        }
        if (strcmp(msg.content, "exit") == 0) {
            reply_to_channel(&msg, "Goodbye! (agent keeps running)");
            continue;
        }

        /* 对话层：调 LLM（核心任务是唯一调用者，天然串行）*/
        char *reply = calloc(1, AGENT_REPLY_SIZE);
        if (!reply) {
            reply_to_channel(&msg, "Error: out of memory");
            continue;
        }
        if (msg.channel == CH_SERIAL) {
            serial_console_printf("Sending to LLM...\n");
        }
        esp_err_t err = llm_client_chat(msg.content, reply, AGENT_REPLY_SIZE);
        if (err == ESP_OK) {
            reply_to_channel(&msg, reply);
        } else {
            char err_buf[128];
            snprintf(err_buf, sizeof(err_buf),
                     "Error: Failed to get LLM reply (%s)", esp_err_to_name(err));
            reply_to_channel(&msg, err_buf);
        }
        free(reply);
    }
}

esp_err_t agent_core_start(void)
{
    if (s_agent_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    s_agent_queue = xQueueCreate(AGENT_QUEUE_LEN, sizeof(agent_msg_t));
    if (!s_agent_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(agent_core_task, "agent_core",
                                 AGENT_TASK_STACK, NULL, 3, NULL);
    if (ret != pdPASS) {
        vQueueDelete(s_agent_queue);
        s_agent_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Agent core started");
    return ESP_OK;
}
