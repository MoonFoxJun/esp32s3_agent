#include "qq_channel.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "agent_core.h"

static const char *TAG = "qq_channel";

/* ========== QQ 机器人凭证（QQ 开放平台 q.qq.com 开发者后台获取，勿公开！）========== */
#define QQ_APP_ID        "YOUR_QQ_APP_ID"
#define QQ_APP_SECRET    "YOUR_QQ_APP_SECRET"

#define QQ_TOKEN_URL     "https://bots.qq.com/app/getAppAccessToken"
#define QQ_API_BASE      "https://api.sgroup.qq.com"

/* WebSocket 协议 op 码 */
#define QQ_OP_DISPATCH        0
#define QQ_OP_HEARTBEAT       1
#define QQ_OP_IDENTIFY        2
#define QQ_OP_RECONNECT       7
#define QQ_OP_INVALID_SESSION 9
#define QQ_OP_HELLO           10
#define QQ_OP_HEARTBEAT_ACK   11
#define QQ_INTENTS            ((1 << 30) | (1 << 25))  /* 群聊 + 单聊事件 */

#define QQ_WS_STACK          6144  /* websocket 任务栈：体检显示 4096 时余量<1KB，加大留余量 */
#define QQ_WS_BUFFER         4096
#define QQ_HEARTBEAT_DEFAULT_MS  30000
#define QQ_RECONNECT_DELAY_MS    5000
#define QQ_TOKEN_EXPIRE_MARGIN_MS 60000   /* token 过期前 60s 提前刷新 */
#define QQ_DEDUP_SLOTS       8

typedef struct {
    char access_token[256];
    int64_t token_expire_ms;          /* esp_timer 毫秒时间戳 */
    char ws_url[384];
    esp_websocket_client_handle_t ws;
    TaskHandle_t ws_task;
    volatile bool identify_pending;
    volatile bool connected;              /* 事件处理器维护的连接状态 */
    volatile int heartbeat_ms;
    volatile int last_seq;
    SemaphoreHandle_t lock;
    /* 消息去重（断线重连时 QQ 可能重投）*/
    char dedup_ids[QQ_DEDUP_SLOTS][64];
    int dedup_idx;
} qq_state_t;

static qq_state_t s_qq;

/* ─── 简单 HTTP 响应收集 ─── */

typedef struct { char *buf; size_t len; size_t cap; } http_resp_t;

static esp_err_t http_on_data(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (!r || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (r->len + (size_t)evt->data_len + 1 > r->cap) {
        size_t nc = r->cap * 2 > r->len + (size_t)evt->data_len + 1
                  ? r->cap * 2 : r->len + (size_t)evt->data_len + 1;
        char *nb = realloc(r->buf, nc);
        if (!nb) return ESP_OK;
        r->buf = nb;
        r->cap = nc;
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += (size_t)evt->data_len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

/* 通用 HTTP 请求：method 0=GET 1=POST；返回 200 且 resp 非空则 ESP_OK */
static esp_err_t qq_http(const char *url, int method, const char *body,
                         const char *auth, char **out_body)
{
    http_resp_t resp = { .buf = NULL, .len = 0, .cap = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_on_data,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    if (method) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (auth) {
        esp_http_client_set_header(c, "Authorization", auth);
    }
    if (body) {
        esp_http_client_set_post_field(c, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200 || !resp.buf) {
        ESP_LOGW(TAG, "HTTP %s %s failed: err=%s status=%d body=%s",
                 method ? "POST" : "GET", url, esp_err_to_name(err), status,
                 resp.buf ? resp.buf : "");
        free(resp.buf);
        return ESP_FAIL;
    }
    *out_body = resp.buf;   /* 调用方负责 free */
    return ESP_OK;
}

/* ─── 换取 access_token ─── */

static esp_err_t qq_get_access_token(void)
{
    char body[320];
    snprintf(body, sizeof(body), "{\"appId\":\"%s\",\"clientSecret\":\"%s\"}",
             QQ_APP_ID, QQ_APP_SECRET);

    char *resp = NULL;
    if (qq_http(QQ_TOKEN_URL, 1, body, NULL, &resp) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;

    cJSON *token = cJSON_GetObjectItem(root, "access_token");
    cJSON *expires = cJSON_GetObjectItem(root, "expires_in");
    if (!cJSON_IsString(token) || strlen(token->valuestring) == 0) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No access_token in response");
        return ESP_FAIL;
    }

    int expire_s = cJSON_IsNumber(expires) ? expires->valueint : 7200;
    if (s_qq.lock) xSemaphoreTake(s_qq.lock, portMAX_DELAY);
    strlcpy(s_qq.access_token, token->valuestring, sizeof(s_qq.access_token));
    s_qq.token_expire_ms = esp_timer_get_time() / 1000
                         + (int64_t)expire_s * 1000 - QQ_TOKEN_EXPIRE_MARGIN_MS;
    if (s_qq.lock) xSemaphoreGive(s_qq.lock);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Access token acquired (expires in %ds)", expire_s);
    return ESP_OK;
}

/* 令牌是否仍有效（未过期）*/
static bool qq_token_valid(void)
{
    return s_qq.access_token[0] != '\0'
        && esp_timer_get_time() / 1000 < s_qq.token_expire_ms;
}

/* ─── 获取 WebSocket 网关地址 ─── */

static esp_err_t qq_fetch_gateway_url(void)
{
    char auth[300];
    snprintf(auth, sizeof(auth), "QQBot %s", s_qq.access_token);

    char *resp = NULL;
    if (qq_http(QQ_API_BASE "/gateway", 0, NULL, auth, &resp) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;

    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url) || strlen(url->valuestring) == 0) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No gateway url in response");
        return ESP_FAIL;
    }
    strlcpy(s_qq.ws_url, url->valuestring, sizeof(s_qq.ws_url));
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Gateway: %s", s_qq.ws_url);
    return ESP_OK;
}

/* ─── WS 发送 ─── */

static esp_err_t qq_ws_send_json(const char *json)
{
    if (!s_qq.ws || !json) return ESP_ERR_INVALID_STATE;
    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_qq.ws, json, len, pdMS_TO_TICKS(1000));
    return (sent == len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t qq_ws_send_identify(void)
{
    char auth[300];
    snprintf(auth, sizeof(auth), "QQBot %s", s_qq.access_token);

    char *json = NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) { cJSON_Delete(root); cJSON_Delete(data); return ESP_ERR_NO_MEM; }
    cJSON_AddNumberToObject(root, "op", QQ_OP_IDENTIFY);
    cJSON_AddStringToObject(data, "token", auth);
    cJSON_AddNumberToObject(data, "intents", QQ_INTENTS);
    cJSON_AddItemToObject(root, "d", data);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = qq_ws_send_json(json);
    free(json);
    return err;
}

static esp_err_t qq_ws_send_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "op", QQ_OP_HEARTBEAT);
    if (s_qq.last_seq > 0) {
        cJSON_AddNumberToObject(root, "d", s_qq.last_seq);
    } else {
        cJSON_AddNullToObject(root, "d");
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = qq_ws_send_json(json);
    free(json);
    return err;
}

/* ─── 消息去重 ─── */

static bool qq_dedup_check(const char *id)
{
    for (int i = 0; i < QQ_DEDUP_SLOTS; i++) {
        if (s_qq.dedup_ids[i][0] && strcmp(s_qq.dedup_ids[i], id) == 0) {
            return true;
        }
    }
    strlcpy(s_qq.dedup_ids[s_qq.dedup_idx], id, sizeof(s_qq.dedup_ids[0]));
    s_qq.dedup_idx = (s_qq.dedup_idx + 1) % QQ_DEDUP_SLOTS;
    return false;
}

/* ─── 处理服务端下发的消息事件 ─── */

static void qq_handle_dispatch(const char *frame)
{
    cJSON *root = cJSON_Parse(frame);
    if (!root) return;

    cJSON *t = cJSON_GetObjectItem(root, "t");
    cJSON *d = cJSON_GetObjectItem(root, "d");
    if (!cJSON_IsString(t) || !d) {
        cJSON_Delete(root);
        return;
    }
    const char *event = t->valuestring;

    const char *chat_id = NULL;   /* 回发目标 */
    const char *msg_id = NULL;
    const char *content = NULL;

    if (strcmp(event, "C2C_MESSAGE_CREATE") == 0) {
        cJSON *author = cJSON_GetObjectItem(d, "author");
        cJSON *openid = author ? cJSON_GetObjectItem(author, "user_openid") : NULL;
        cJSON *mid = cJSON_GetObjectItem(d, "id");
        cJSON *ct = cJSON_GetObjectItem(d, "content");
        if (cJSON_IsString(openid) && cJSON_IsString(ct)) {
            msg_id = cJSON_IsString(mid) ? mid->valuestring : "";
            content = ct->valuestring;
            char *cid = malloc(64 + strlen(openid->valuestring) + 1);
            if (cid) {
                snprintf(cid, 64 + strlen(openid->valuestring) + 1, "c2c_%s", openid->valuestring);
                chat_id = cid;
            }
        }
    } else if (strcmp(event, "GROUP_AT_MESSAGE_CREATE") == 0) {
        cJSON *gid = cJSON_GetObjectItem(d, "group_openid");
        cJSON *mid = cJSON_GetObjectItem(d, "id");
        cJSON *ct = cJSON_GetObjectItem(d, "content");
        if (cJSON_IsString(gid) && cJSON_IsString(ct)) {
            msg_id = cJSON_IsString(mid) ? mid->valuestring : "";
            content = ct->valuestring;
            char *cid = malloc(64 + strlen(gid->valuestring) + 1);
            if (cid) {
                snprintf(cid, 64 + strlen(gid->valuestring) + 1, "grp_%s", gid->valuestring);
                chat_id = cid;
            }
        }
    }

    if (chat_id && content) {
        if (!qq_dedup_check(msg_id && msg_id[0] ? msg_id : content)) {
            agent_msg_t msg = { .channel = CH_QQ };
            strlcpy(msg.session_id, chat_id, sizeof(msg.session_id));
            strlcpy(msg.content, content, sizeof(msg.content));
            agent_core_submit(&msg);
        }
    }
    free((void *)chat_id);
    cJSON_Delete(root);
}

/* ─── WS 事件回调（在 ws 客户端任务上下文执行）─── */

static void qq_ws_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WS connected");
        s_qq.connected = true;
        s_qq.identify_pending = true;
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "WS disconnected");
        s_qq.connected = false;
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)event_data;
        if (!evt || evt->op_code != 0x01 /* text frame */ || !evt->data_ptr || evt->data_len <= 0) {
            return;
        }
        /* 分片防护：QQ 消息通常单帧到达；分片时丢弃（可后续补组装）*/
        if (evt->payload_offset != 0 || evt->payload_len != evt->data_len) {
            ESP_LOGW(TAG, "Fragmented WS frame ignored (off=%d len=%d/%d)",
                     evt->payload_offset, evt->data_len, evt->payload_len);
            return;
        }
        char *frame = malloc((size_t)evt->data_len + 1);
        if (!frame) return;
        memcpy(frame, evt->data_ptr, (size_t)evt->data_len);
        frame[evt->data_len] = '\0';

        /* 解析 op 码 */
        cJSON *root = cJSON_Parse(frame);
        if (root) {
            cJSON *op = cJSON_GetObjectItem(root, "op");
            if (cJSON_IsNumber(op)) {
                int code = op->valueint;
                if (code == QQ_OP_HELLO) {
                    cJSON *d = cJSON_GetObjectItem(root, "d");
                    cJSON *hi = d ? cJSON_GetObjectItem(d, "heartbeat_interval") : NULL;
                    s_qq.heartbeat_ms = cJSON_IsNumber(hi) ? hi->valueint
                                                            : QQ_HEARTBEAT_DEFAULT_MS;
                    ESP_LOGI(TAG, "WS hello, heartbeat=%dms", s_qq.heartbeat_ms);
                    /* 服务器要求连上后尽快 identify，超时会被断开：
                     * 直接在事件处理器里发，不等心跳循环 */
                    s_qq.identify_pending = false;
                    if (qq_ws_send_identify() == ESP_OK) {
                        ESP_LOGI(TAG, "Identify sent");
                    }
                } else if (code == QQ_OP_HEARTBEAT_ACK) {
                    /* 心跳确认，无需处理 */
                } else if (code == QQ_OP_DISPATCH) {
                    cJSON *s = cJSON_GetObjectItem(root, "s");
                    if (cJSON_IsNumber(s)) s_qq.last_seq = s->valueint;
                    cJSON_Delete(root);
                    qq_handle_dispatch(frame);   /* 先处理再释放 frame */
                    free(frame);
                    return;
                } else if (code == QQ_OP_RECONNECT || code == QQ_OP_INVALID_SESSION) {
                    ESP_LOGW(TAG, "WS op %d: re-identify", code);
                    qq_ws_send_identify();
                }
            }
            cJSON_Delete(root);
        }
        free(frame);
    }
}

/* ─── 连接任务（换 token → 拿网关 → 连 WS → 心跳/重连）─── */

static void qq_ws_task(void *arg)
{
    (void)arg;
    while (1) {
        if (!qq_token_valid() && qq_get_access_token() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(QQ_RECONNECT_DELAY_MS));
            continue;
        }
        if (qq_fetch_gateway_url() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(QQ_RECONNECT_DELAY_MS));
            continue;
        }

        esp_websocket_client_config_t ws_cfg = {
            .uri = s_qq.ws_url,
            .buffer_size = QQ_WS_BUFFER,
            .task_stack = QQ_WS_STACK,
            .network_timeout_ms = 10000,
            .reconnect_timeout_ms = QQ_RECONNECT_DELAY_MS,
            .disable_auto_reconnect = true,   /* 自己控制重连 */
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_qq.ws = esp_websocket_client_init(&ws_cfg);
        if (!s_qq.ws) {
            vTaskDelay(pdMS_TO_TICKS(QQ_RECONNECT_DELAY_MS));
            continue;
        }
        s_qq.heartbeat_ms = QQ_HEARTBEAT_DEFAULT_MS;
        s_qq.connected = false;
        s_qq.identify_pending = false;
        esp_websocket_register_events(s_qq.ws, WEBSOCKET_EVENT_ANY,
                                      qq_ws_event_handler, NULL);
        esp_websocket_client_start(s_qq.ws);

        /* 等待连接建立（最多 15s；identify 已在事件处理器里发过）*/
        for (int w = 0; w < 75 && s_qq.ws && !s_qq.connected; w++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        /* 心跳循环：只在连接期间跑 */
        TickType_t last_hb = xTaskGetTickCount();
        while (s_qq.ws && s_qq.connected) {
            /* token 快过期时提前刷新（QQ token 有效期 2 小时；
             * 之前只在断线重连时刷新，长连接 2 小时后所有回复会静默失败）*/
            if (!qq_token_valid() && qq_get_access_token() != ESP_OK) {
                ESP_LOGW(TAG, "token refresh failed, retry later");
                vTaskDelay(pdMS_TO_TICKS(QQ_RECONNECT_DELAY_MS));
                continue;
            }
            if ((xTaskGetTickCount() - last_hb) >= pdMS_TO_TICKS(s_qq.heartbeat_ms / 2)) {
                qq_ws_send_heartbeat();
                last_hb = xTaskGetTickCount();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        esp_websocket_client_stop(s_qq.ws);
        esp_websocket_client_destroy(s_qq.ws);
        s_qq.ws = NULL;
        s_qq.connected = false;
        ESP_LOGW(TAG, "WS closed, reconnecting...");
        vTaskDelay(pdMS_TO_TICKS(QQ_RECONNECT_DELAY_MS));
    }
}

/* ─── 回发消息 ─── */

#define QQ_MAX_MSG_LEN  1500   /* QQ 单条消息内容上限（esp-claw 同款限制）*/

/* UTF-8 安全截断：不超过 max_len 字节，且不在多字节字符（如中文）中间断开 */
static int qq_utf8_prefix_len(const char *s, int max_len)
{
    int len = 0;
    while (len < max_len && s[len]) {
        unsigned char c = (unsigned char)s[len];
        int char_len = 1;
        if (c >= 0xF0) char_len = 4;
        else if (c >= 0xE0) char_len = 3;
        else if (c >= 0xC0) char_len = 2;
        if (len + char_len > max_len) {
            break;   /* 放不下整个字符，留到下一段 */
        }
        len += char_len;
    }
    return len;
}

/* 发送单段消息（HTTP POST）*/
static esp_err_t qq_send_chunk(const char *chat_id, const char *text)
{
    char url[512];
    if (strncmp(chat_id, "c2c_", 4) == 0) {
        snprintf(url, sizeof(url), "%s/v2/users/%s/messages", QQ_API_BASE, chat_id + 4);
    } else if (strncmp(chat_id, "grp_", 4) == 0) {
        snprintf(url, sizeof(url), "%s/v2/groups/%s/messages", QQ_API_BASE, chat_id + 4);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    char body[QQ_MAX_MSG_LEN + 128];
    cJSON *b = cJSON_CreateObject();
    if (!b) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(b, "content", text);
    char *body_json = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body_json) return ESP_ERR_NO_MEM;
    strlcpy(body, body_json, sizeof(body));
    free(body_json);

    char auth[300];
    snprintf(auth, sizeof(auth), "QQBot %s", s_qq.access_token);

    char *resp = NULL;
    esp_err_t err = qq_http(url, 1, body, auth, &resp);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "QQ reply sent to %s", chat_id);
    }
    free(resp);
    return err;
}

esp_err_t qq_channel_send_text(const char *chat_id, const char *text)
{
    if (!chat_id || !text) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!qq_token_valid()) {
        /* 之前这里是静默返回：token 失效时用户只看到"没回复"，无从排查 */
        ESP_LOGW(TAG, "send skipped: access token invalid/expired");
        return ESP_ERR_INVALID_STATE;
    }

    /* 长回复分段发送（QQ 单条有长度上限；按 UTF-8 字符边界切，不切坏中文）*/
    int total = (int)strlen(text);
    int offset = 0;
    while (offset < total) {
        int chunk_len = total - offset;
        if (chunk_len > QQ_MAX_MSG_LEN) {
            chunk_len = qq_utf8_prefix_len(text + offset, QQ_MAX_MSG_LEN);
        }
        char chunk[QQ_MAX_MSG_LEN + 1];
        memcpy(chunk, text + offset, (size_t)chunk_len);
        chunk[chunk_len] = '\0';

        esp_err_t err = qq_send_chunk(chat_id, chunk);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk_len;
    }
    return ESP_OK;
}

esp_err_t qq_channel_start(void)
{
    s_qq.lock = xSemaphoreCreateMutex();
    if (!s_qq.lock) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(qq_ws_task, "qq_ch", QQ_WS_STACK + 2048, NULL, 4, &s_qq.ws_task)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "QQ channel started (app_id=%s)", QQ_APP_ID);
    return ESP_OK;
}
