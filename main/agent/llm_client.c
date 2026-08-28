#include "llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "agent_tools.h"

static const char *TAG = "llm_client";
#define LLM_RESPONSE_BUFFER_SIZE  4096
#define LLM_MAX_TOOL_ITERATIONS   5     /* 工具调用循环上限 */
#define LLM_TOOL_RESULT_SIZE      512

/* ─── Phase 5: NVS 持久化 ─── */
#define CHAT_NVS_NS        "chat"
#define CHAT_NVS_COUNT     "count"
#define CHAT_NVS_PROMPT    "prompt"
#define CHAT_NVS_MSG_PREFIX "msg_"
#define CHAT_NVS_MAX_MSG   3500   /* 单条消息超过此长度不持久化（NVS 单键 ~4000B 限制）*/

typedef struct {
    char *base_url;
    char *api_key;
    char *model;
    char *system_prompt;          /* System Prompt（可选）*/
} llm_client_t;

static llm_client_t s_client = {0};

static bool s_has_saved_prompt = false;   /* NVS 里是否有用户保存过的人设 */

/* 消息历史：完整 message 对象 JSON 字符串数组（滚动窗口）
 * 存完整 JSON 而不是 {role, content} 对，是为了能存带 tool_calls 的消息 */
static char *s_history[LLM_MAX_HISTORY];
static int s_history_count;

/* ─── 历史管理 ─── */

static void history_clear(void)
{
    for (int i = 0; i < s_history_count; i++) {
        free(s_history[i]);
        s_history[i] = NULL;
    }
    s_history_count = 0;
}

static esp_err_t history_append(const char *msg_json)
{
    if (!msg_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_history_count >= LLM_MAX_HISTORY) {
        /* 滚动：移除最旧一条 */
        free(s_history[0]);
        memmove(&s_history[0], &s_history[1], (LLM_MAX_HISTORY - 1) * sizeof(char *));
        s_history_count--;
    }
    s_history[s_history_count] = strdup(msg_json);
    if (!s_history[s_history_count]) {
        return ESP_ERR_NO_MEM;
    }
    s_history_count++;
    return ESP_OK;
}

/* ─── Phase 5: NVS 持久化（保存/恢复 人设 + 历史）─── */

static esp_err_t persist_chat_state(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CHAT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_client.system_prompt) {
        nvs_set_str(h, CHAT_NVS_PROMPT, s_client.system_prompt);
    } else {
        nvs_erase_key(h, CHAT_NVS_PROMPT);
    }

    nvs_set_i32(h, CHAT_NVS_COUNT, s_history_count);
    for (int i = 0; i < s_history_count; i++) {
        if ((int)strlen(s_history[i]) > CHAT_NVS_MAX_MSG) {
            ESP_LOGW(TAG, "History msg %d too long (%d B), skip persist", i,
                     (int)strlen(s_history[i]));
            continue;
        }
        char key[16];
        snprintf(key, sizeof(key), "%s%d", CHAT_NVS_MSG_PREFIX, i);
        nvs_set_str(h, key, s_history[i]);
    }
    /* 清理多余的旧键（历史变短时）*/
    for (int i = s_history_count; i < LLM_MAX_HISTORY; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", CHAT_NVS_MSG_PREFIX, i);
        nvs_erase_key(h, key);
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Chat state persisted (%d messages)", s_history_count);
    return ESP_OK;
}

static void restore_chat_state(void)
{
    nvs_handle_t h;
    if (nvs_open(CHAT_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;  /* 首次启动，无存档 */
    }

    /* System Prompt */
    size_t len = 0;
    if (nvs_get_str(h, CHAT_NVS_PROMPT, NULL, &len) == ESP_OK && len > 1) {
        char *buf = malloc(len);
        if (buf) {
            if (nvs_get_str(h, CHAT_NVS_PROMPT, buf, &len) == ESP_OK) {
                free(s_client.system_prompt);
                s_client.system_prompt = buf;
                s_has_saved_prompt = true;   /* 有存档：开机用它，不用默认 */
                buf = NULL;
            }
            free(buf);
        }
    }

    /* 历史消息 */
    int32_t count = 0;
    if (nvs_get_i32(h, CHAT_NVS_COUNT, &count) == ESP_OK && count > 0) {
        if (count > LLM_MAX_HISTORY) {
            count = LLM_MAX_HISTORY;
        }
        for (int32_t i = 0; i < count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%s%d", CHAT_NVS_MSG_PREFIX, (int)i);
            size_t mlen = 0;
            if (nvs_get_str(h, key, NULL, &mlen) == ESP_OK && mlen > 1) {
                char *buf = malloc(mlen);
                if (buf) {
                    if (nvs_get_str(h, key, buf, &mlen) == ESP_OK) {
                        history_append(buf);
                    }
                    free(buf);
                }
            }
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Chat state restored: prompt=%s messages=%d",
             s_client.system_prompt ? "yes" : "no", s_history_count);
}

/* ─── JSON 转义（手写，用于构建请求体）─── */

static int json_escape(const char *src, char *dst, int dst_size)
{
    int i = 0;
    /* 只保留 1 字节给结尾 '\0'；调用方按转义后长度+1 分配，不能多留余量，
     * 否则会截掉最后一个多字节 UTF-8 字符（曾导致 API 报 invalid unicode）*/
    while (*src && i < dst_size - 1) {
        switch (*src) {
        case '"':  dst[i++] = '\\'; dst[i++] = '"'; break;
        case '\\': dst[i++] = '\\'; dst[i++] = '\\'; break;
        case '\n': dst[i++] = '\\'; dst[i++] = 'n'; break;
        case '\r': dst[i++] = '\\'; dst[i++] = 'r'; break;
        case '\t': dst[i++] = '\\'; dst[i++] = 't'; break;
        case '\b': dst[i++] = '\\'; dst[i++] = 'b'; break;
        case '\f': dst[i++] = '\\'; dst[i++] = 'f'; break;
        default:
            if ((unsigned char)*src >= 0x20) {
                dst[i++] = *src;
            }
            break;
        }
        src++;
    }
    dst[i] = '\0';
    return i;
}

static int json_escaped_len(const char *src)
{
    int len = 0;
    while (*src) {
        switch (*src) {
        case '"': case '\\': case '\n': case '\r': case '\t':
        case '\b': case '\f':
            len += 2;
            break;
        default:
            if ((unsigned char)*src >= 0x20) {
                len += 1;
            }
            break;
        }
        src++;
    }
    return len;
}

/* ─── 构建请求体 ───
 * user_message 为 NULL 表示工具循环的后续迭代（不带新的用户消息）*/
static char *build_request_body(const char *user_message)
{
    int total = 256;
    if (user_message) {
        total += json_escaped_len(user_message);
    }
    if (s_client.system_prompt) {
        total += 64 + json_escaped_len(s_client.system_prompt);
    }
    for (int i = 0; i < s_history_count; i++) {
        total += (int)strlen(s_history[i]) + 8;
    }
    const char *tools_json = agent_tools_get_json();
    if (tools_json) {
        total += (int)strlen(tools_json) + 32;
    }

    char *b = malloc(total);
    if (!b) {
        return NULL;
    }

    int pos = 0;
    int wrote = 0;
    pos += snprintf(b + pos, total - pos, "{\"model\":\"%s\",\"messages\":[", s_client.model);

    /* System Prompt */
    if (s_client.system_prompt) {
        int elen = json_escaped_len(s_client.system_prompt);
        char *escaped = malloc(elen + 1);
        if (!escaped) {
            free(b);
            return NULL;
        }
        json_escape(s_client.system_prompt, escaped, elen + 1);
        pos += snprintf(b + pos, total - pos,
                        "{\"role\":\"system\",\"content\":\"%s\"}", escaped);
        free(escaped);
        wrote = 1;
    }

    /* 历史消息（已是完整 JSON，直接插入）*/
    for (int i = 0; i < s_history_count; i++) {
        pos += snprintf(b + pos, total - pos, "%s%s", wrote ? "," : "", s_history[i]);
        wrote = 1;
    }

    /* 当前用户消息（仅第一轮）*/
    if (user_message) {
        int elen = json_escaped_len(user_message);
        char *escaped = malloc(elen + 1);
        if (!escaped) {
            free(b);
            return NULL;
        }
        json_escape(user_message, escaped, elen + 1);
        pos += snprintf(b + pos, total - pos, "%s{\"role\":\"user\",\"content\":\"%s\"}",
                        wrote ? "," : "", escaped);
        free(escaped);
        wrote = 1;
    }

    pos += snprintf(b + pos, total - pos, "]");

    /* 工具定义 */
    if (tools_json) {
        pos += snprintf(b + pos, total - pos, ",\"tools\":%s", tools_json);
    }

    pos += snprintf(b + pos, total - pos, "}");
    return b;
}

/* ─── HTTP event handler（响应体分块收集）─── */

typedef struct { char *buf; size_t len; size_t cap; } response_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_ctx_t *ctx = (response_ctx_t *)evt->user_data;
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t nl = ctx->len + evt->data_len;
        if (nl + 1 > ctx->cap) {
            size_t nc = ctx->cap * 2 > nl + 1 ? ctx->cap * 2 : nl + 1;
            char *nb = realloc(ctx->buf, nc);
            if (!nb) return ESP_OK;
            ctx->buf = nb;
            ctx->cap = nc;
        }
        memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
        ctx->len = nl;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

/* ─── HTTP request task（8KB 栈，只负责收发，不解析）─── */

typedef struct {
    char *url;
    char *auth_header;
    char *body;
    response_ctx_t *ctx;
    EventGroupHandle_t done;
    esp_err_t result;
    int http_status;
} http_args_t;

static void http_request_task(void *arg)
{
    http_args_t *a = (http_args_t *)arg;
    esp_err_t err = ESP_FAIL;

    esp_http_client_config_t config = {0};
    config.url = a->url;
    config.timeout_ms = 30000;   /* 30s：请求随历史/工具变大，DeepSeek 思考变慢，10s 会误杀 */
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.event_handler = http_event_handler;
    config.user_data = a->ctx;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Authorization", a->auth_header);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, a->body, (int)strlen(a->body));
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            a->http_status = esp_http_client_get_status_code(client);
        }
        esp_http_client_cleanup(client);
    }

    a->result = err;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP task failed: status=%d err=%s",
                 a->http_status, esp_err_to_name(err));
    }
    xEventGroupSetBits(a->done, 1);
    vTaskDelete(NULL);
}

/* ─── 响应解析（cJSON）：提取 content 和 tool_calls ─── */

typedef struct {
    char *assistant_msg_json; /* 完整 assistant 消息 JSON（含 tool_calls）*/
    char *content;            /* 最终回答（无 tool_calls 时）*/
    char **call_ids;          /* tool_calls: id 数组 */
    char **call_names;        /* 函数名 */
    char **call_args;         /* 参数 JSON 字符串 */
    int call_count;
} llm_response_t;

static void llm_response_free(llm_response_t *r)
{
    if (!r) return;
    free(r->assistant_msg_json);
    free(r->content);
    for (int i = 0; i < r->call_count; i++) {
        free(r->call_ids[i]);
        free(r->call_names[i]);
        free(r->call_args[i]);
    }
    free(r->call_ids);
    free(r->call_names);
    free(r->call_args);
    memset(r, 0, sizeof(*r));
}

static esp_err_t parse_response(const char *body, llm_response_t *r)
{
    if (!body || !r) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "Response JSON parse failed");
        return ESP_FAIL;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *first = (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
    if (!msg) {
        ESP_LOGE(TAG, "No choices[0].message in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* 完整 assistant 消息原样保留（历史需要，含 tool_calls 结构）*/
    r->assistant_msg_json = cJSON_PrintUnformatted(msg);

    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content)) {
        r->content = strdup(content->valuestring);
    }

    cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
    if (calls && cJSON_IsArray(calls)) {
        r->call_count = cJSON_GetArraySize(calls);
        r->call_ids = calloc(r->call_count, sizeof(char *));
        r->call_names = calloc(r->call_count, sizeof(char *));
        r->call_args = calloc(r->call_count, sizeof(char *));
        int idx = 0;
        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, calls) {
            cJSON *id = cJSON_GetObjectItem(tc, "id");
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            if (cJSON_IsString(id))   r->call_ids[idx]   = strdup(id->valuestring);
            if (cJSON_IsString(name)) r->call_names[idx] = strdup(name->valuestring);
            if (cJSON_IsString(args)) r->call_args[idx]  = strdup(args->valuestring);
            idx++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ─── 构造 message 对象 JSON（cJSON 自动转义）─── */
static char *make_message_json(const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    if (!m) return NULL;
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content ? content : "");
    char *json = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    return json;
}

/* ─── Public API ─── */

esp_err_t llm_client_init(const char *base_url, const char *api_key, const char *model)
{
    if (!base_url || !api_key || !model) return ESP_ERR_INVALID_ARG;
    s_client.base_url = strdup(base_url);
    s_client.api_key = strdup(api_key);
    s_client.model = strdup(model);
    if (!s_client.base_url || !s_client.api_key || !s_client.model) {
        free(s_client.base_url); free(s_client.api_key); free(s_client.model);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LLM client initialized: model=%s", model);
    restore_chat_state();  /* Phase 5: 恢复上次会话（人设 + 历史）*/
    return ESP_OK;
}

esp_err_t llm_client_set_system_prompt(const char *prompt)
{
    free(s_client.system_prompt);
    if (prompt) {
        s_client.system_prompt = strdup(prompt);
        if (!s_client.system_prompt) return ESP_ERR_NO_MEM;
        ESP_LOGI(TAG, "System prompt set: %.128s...", prompt);
    } else {
        s_client.system_prompt = NULL;
        ESP_LOGI(TAG, "System prompt cleared");
    }
    persist_chat_state();  /* Phase 5: 人设变更即存档 */
    return ESP_OK;
}

bool llm_client_has_saved_prompt(void)
{
    return s_has_saved_prompt;
}

void llm_client_clear_history(void)
{
    history_clear();
    persist_chat_state();  /* Phase 5: 清空即存档 */
    ESP_LOGI(TAG, "History cleared");
}

esp_err_t llm_client_chat(const char *user_message, char *reply, size_t reply_size)
{
    if (!user_message || !reply || !reply_size) return ESP_ERR_INVALID_ARG;
    if (!s_client.base_url || !s_client.api_key || !s_client.model) {
        ESP_LOGE(TAG, "LLM client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_FAIL;
    bool timed_out = false;
    bool done = false;

    for (int iter = 0; iter < LLM_MAX_TOOL_ITERATIONS && !done; iter++) {
        /* 1. 构建请求体（第一轮带用户消息，后续迭代只带历史+tools）*/
        char *body = build_request_body(iter == 0 ? user_message : NULL);
        if (!body) return ESP_ERR_NO_MEM;

        char *url = malloc(256);
        char *auth = malloc(256);
        char *rbuf = calloc(1, LLM_RESPONSE_BUFFER_SIZE);
        response_ctx_t *ctx = calloc(1, sizeof(response_ctx_t));
        if (!url || !auth || !rbuf || !ctx) {
            free(body); free(url); free(auth); free(rbuf); free(ctx);
            return ESP_ERR_NO_MEM;
        }
        snprintf(url, 256, "%s/chat/completions", s_client.base_url);
        snprintf(auth, 256, "Bearer %s", s_client.api_key);
        ctx->buf = rbuf; ctx->len = 0; ctx->cap = LLM_RESPONSE_BUFFER_SIZE;

        EventGroupHandle_t done_evt = xEventGroupCreate();
        if (!done_evt) {
            free(body); free(url); free(auth); free(rbuf); free(ctx);
            return ESP_ERR_NO_MEM;
        }

        http_args_t *a = calloc(1, sizeof(http_args_t));
        if (!a) {
            free(body); free(url); free(auth); free(rbuf); free(ctx);
            vEventGroupDelete(done_evt);
            return ESP_ERR_NO_MEM;
        }
        a->url = url; a->auth_header = auth; a->body = body;
        a->ctx = ctx; a->done = done_evt;

        BaseType_t ret = xTaskCreate(http_request_task, "http", 8192, a, 5, NULL);
        if (ret != pdPASS) {
            free(a); free(body); free(url); free(auth); free(rbuf); free(ctx);
            vEventGroupDelete(done_evt);
            return ESP_ERR_NO_MEM;
        }

        EventBits_t bits = xEventGroupWaitBits(done_evt, 1, pdTRUE, pdTRUE, pdMS_TO_TICKS(20000));
        if (!(bits & 1)) {
            /* worker 可能还在 perform() 里：等它收尾再清理，避免对已删除对象置位 */
            ESP_LOGW(TAG, "HTTP request timed out, waiting for worker to finish...");
            xEventGroupWaitBits(done_evt, 1, pdTRUE, pdTRUE, portMAX_DELAY);
            timed_out = true;
        }

        esp_err_t http_err = a->result;
        int status = a->http_status;
        char *resp_body = strdup(ctx->buf ? ctx->buf : "");

        free(a); free(body); free(url); free(auth); free(rbuf); free(ctx);
        vEventGroupDelete(done_evt);

        if (timed_out) {
            free(resp_body);
            result = ESP_ERR_TIMEOUT;
            done = true;
            break;
        }
        if (http_err != ESP_OK || status != 200) {
            ESP_LOGE(TAG, "HTTP failed: status=%d err=%s body=%.256s",
                     status, esp_err_to_name(http_err), resp_body);
            free(resp_body);
            result = (http_err != ESP_OK) ? http_err : ESP_FAIL;
            done = true;
            break;
        }

        /* 2. 解析响应 */
        llm_response_t resp = {0};
        if (parse_response(resp_body, &resp) != ESP_OK) {
            ESP_LOGE(TAG, "Response parse failed: %.256s", resp_body);
            free(resp_body);
            result = ESP_FAIL;
            done = true;
            break;
        }
        free(resp_body);

        if (resp.call_count > 0) {
            /* 3a. 工具调用：assistant 消息入史 → 执行工具 → tool 结果入史 → 再问一轮 */
            if (resp.assistant_msg_json) {
                history_append(resp.assistant_msg_json);
            }
            for (int i = 0; i < resp.call_count; i++) {
                char out[LLM_TOOL_RESULT_SIZE];
                esp_err_t terr = agent_tools_execute(
                    resp.call_names[i] ? resp.call_names[i] : "?",
                    resp.call_args[i] ? resp.call_args[i] : "{}",
                    out, sizeof(out));
                if (terr != ESP_OK && out[0] == '\0') {
                    snprintf(out, sizeof(out), "tool error: %s", esp_err_to_name(terr));
                }

                /* tool 结果消息：{"role":"tool","tool_call_id":...,"content":...} */
                cJSON *tm = cJSON_CreateObject();
                if (tm) {
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id",
                                            resp.call_ids[i] ? resp.call_ids[i] : "");
                    cJSON_AddStringToObject(tm, "content", out);
                    char *tj = cJSON_PrintUnformatted(tm);
                    cJSON_Delete(tm);
                    if (tj) {
                        history_append(tj);
                        free(tj);
                    }
                }
            }
            llm_response_free(&resp);
            continue;  /* 下一轮迭代，不带用户消息 */
        }

        /* 3b. 无工具调用：最终回答 */
        if (resp.content) {
            strlcpy(reply, resp.content, reply_size);
            /* 本轮对话入史（供下一次请求使用）*/
            char *uj = make_message_json("user", user_message);
            if (uj) {
                history_append(uj);
                free(uj);
            }
            if (resp.assistant_msg_json) {
                history_append(resp.assistant_msg_json);
            }
            result = ESP_OK;
        } else {
            ESP_LOGW(TAG, "Empty response content");
            result = ESP_FAIL;
        }
        llm_response_free(&resp);
        done = true;
    }

    if (!done) {
        ESP_LOGE(TAG, "Tool loop reached max iterations (%d)", LLM_MAX_TOOL_ITERATIONS);
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "LLM reply: %s", reply);
        persist_chat_state();  /* Phase 5: 对话成功即入 NVS */
    }
    return result;
}

void llm_client_deinit(void)
{
    free(s_client.base_url);
    free(s_client.api_key);
    free(s_client.model);
    free(s_client.system_prompt);
    history_clear();
    memset(&s_client, 0, sizeof(s_client));
    ESP_LOGI(TAG, "LLM client deinitialized");
}
