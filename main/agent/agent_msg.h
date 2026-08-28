#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通道类型：新增通道时在此追加，并在 agent_core 的回复分发处补一条 */
typedef enum {
    CH_SERIAL = 0,   /* 串口控制台（UART0）*/
    CH_QQ,           /* QQ 机器人（Phase 4.4）*/
    CH_MAX
} channel_t;

/* 统一消息：所有通道的输入都转成这个结构，进同一个队列 */
typedef struct {
    channel_t   channel;        /* 从哪个通道来，就回哪个通道 */
    char        session_id[64]; /* 会话键："uart" / "c2c_<openid>" / "grp_<openid>"
                                 * QQ openid 有 32 位，前缀+id 需要 >32，所以留 64 */
    char        content[512];   /* 输入文本 */
} agent_msg_t;

#ifdef __cplusplus
}
#endif
