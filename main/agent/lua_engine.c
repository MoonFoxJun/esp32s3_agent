#include "lua_engine.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "led_ctrl.h"

static const char *TAG = "lua_engine";

/* 前向声明：后台脚本区在文件末尾定义 */
static esp_err_t lua_stop_bg_wait(void);

/* Lua 分配器：全部走 PSRAM。
 * Lua 结构只被 CPU 访问（无 DMA），PSRAM 完全够用；
 * 一个 Lua 状态（解释器+库）约 60-80KB，放内部 RAM 会把 TLS/网络挤爆。*/
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    if (!ptr) {
        return heap_caps_malloc(nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static lua_State *lua_newstate_psram(void)
{
    return lua_newstate(lua_psram_alloc, NULL, 0);   /* 第三个参数是哈希随机种子 */
}

/* Lua 状态（全局单例；agent_core 是唯一调用者，天然串行）*/
static lua_State *s_L;

/* ─── 输出捕获（单消费者：agent_core 任务独占调用）─── */
static char *s_out;          /* 本次执行的输出缓冲 */
static size_t s_out_len;
static size_t s_out_cap;

/* ─── 超时控制 ─── */
static uint32_t s_timeout_ms;
static TickType_t s_start_tick;

static void capture_append(const char *s, size_t len)
{
    if (!s_out || !s) {
        return;
    }
    size_t room = s_out_cap - 1 - s_out_len;
    if (len > room) {
        len = room;
    }
    memcpy(s_out + s_out_len, s, len);
    s_out_len += len;
    s_out[s_out_len] = '\0';
}

/* Lua 的 print 重定向到捕获缓冲 */
static int lua_capture_print(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) {
            capture_append(" ", 1);
        }
        capture_append(s, len);
        lua_pop(L, 1);  /* 弹出 tostring 结果 */
    }
    capture_append("\n", 1);
    return 0;
}

/* 超时钩子：每 1000 条指令检查一次耗时，超时则中止脚本 */
static void lua_timeout_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (s_timeout_ms > 0 &&
        (xTaskGetTickCount() - s_start_tick) > pdMS_TO_TICKS(s_timeout_ms)) {
        luaL_error(L, "script execution timeout (%u ms)", (unsigned)s_timeout_ms);
    }
}

/* ─── Lua 硬件绑定 ─── */

/* 颜色名 → RGB（red/orange/yellow/green/cyan/blue/purple/white/off；未知返回 false）*/
static bool lua_parse_color(const char *color, uint32_t *r, uint32_t *g, uint32_t *b)
{
    if (strcmp(color, "red") == 0)    { *r = 255; *g = 0;   *b = 0;   return true; }
    if (strcmp(color, "orange") == 0) { *r = 255; *g = 128; *b = 0;   return true; }
    if (strcmp(color, "yellow") == 0) { *r = 255; *g = 255; *b = 0;   return true; }
    if (strcmp(color, "green") == 0)  { *r = 0;   *g = 255; *b = 0;   return true; }
    if (strcmp(color, "cyan") == 0)   { *r = 0;   *g = 255; *b = 255; return true; }
    if (strcmp(color, "blue") == 0)   { *r = 0;   *g = 0;   *b = 255; return true; }
    if (strcmp(color, "purple") == 0) { *r = 128; *g = 0;   *b = 255; return true; }
    if (strcmp(color, "white") == 0)  { *r = 255; *g = 255; *b = 255; return true; }
    if (strcmp(color, "off") == 0)    { *r = 0;   *g = 0;   *b = 0;   return true; }
    return false;
}

/* 亮度缩放（0-255），并夹取到合法范围 */
static void lua_apply_brightness(int brightness, uint32_t *r, uint32_t *g, uint32_t *b)
{
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;
    *r = *r * (uint32_t)brightness / 255;
    *g = *g * (uint32_t)brightness / 255;
    *b = *b * (uint32_t)brightness / 255;
}

/* led.set(state, color, brightness)：整条灯带变色 */
static int lua_led_set(lua_State *L)
{
    const char *state = luaL_checkstring(L, 1);
    const char *color = luaL_optstring(L, 2, "white");
    int brightness = (int)luaL_optinteger(L, 3, 255);

    uint32_t r, g, b;
    if (!lua_parse_color(color, &r, &g, &b)) {
        lua_pushfstring(L, "unknown color: %s (red/green/blue/white/off)", color);
        return lua_error(L);
    }
    lua_apply_brightness(brightness, &r, &g, &b);

    esp_err_t err = (strcmp(state, "on") == 0) ? led_ctrl_set_all(r, g, b)
                                               : led_ctrl_clear();
    lua_pushstring(L, err == ESP_OK ? "ok" : esp_err_to_name(err));
    return 1;
}

/* led.pixel(index, color, brightness)：点亮单颗灯珠（index 从 1 开始）*/
static int lua_led_pixel(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 1);
    const char *color = luaL_optstring(L, 2, "white");
    int brightness = (int)luaL_optinteger(L, 3, 255);

    uint32_t r, g, b;
    if (!lua_parse_color(color, &r, &g, &b)) {
        lua_pushfstring(L, "unknown color: %s (red/green/blue/white/off)", color);
        return lua_error(L);
    }
    lua_apply_brightness(brightness, &r, &g, &b);

    int count = led_ctrl_get_led_count();
    if (index < 1 || index > count) {
        lua_pushfstring(L, "index must be 1..%d (got %d)", count, index);
        return lua_error(L);
    }

    esp_err_t err = led_ctrl_set_pixel(index - 1, r, g, b);  /* C 层序号从 0 开始 */
    lua_pushstring(L, err == ESP_OK ? "ok" : esp_err_to_name(err));
    return 1;
}

/* HSV → RGB（h:0-360, s/v:0-255）*/
static void hsv_to_rgb(int h, int s, int v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h = h % 360;
    if (h < 0) h += 360;
    int region = h / 60;
    int f = (h % 60) * 255 / 60;
    int p = v * (255 - s) / 255;
    int q = v * (255 - (s * f) / 255) / 255;
    int t = v * (255 - (s * (255 - f)) / 255) / 255;
    switch (region) {
        case 0: *r = (uint32_t)v; *g = (uint32_t)t; *b = (uint32_t)p; break;
        case 1: *r = (uint32_t)q; *g = (uint32_t)v; *b = (uint32_t)p; break;
        case 2: *r = (uint32_t)p; *g = (uint32_t)v; *b = (uint32_t)t; break;
        case 3: *r = (uint32_t)p; *g = (uint32_t)q; *b = (uint32_t)v; break;
        case 4: *r = (uint32_t)t; *g = (uint32_t)p; *b = (uint32_t)v; break;
        default:*r = (uint32_t)v; *g = (uint32_t)p; *b = (uint32_t)q; break;
    }
}

/* led.rgb(index, r, g, b)：单颗灯珠原始 RGB（0-255），最底层控制 */
static int lua_led_rgb(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 1);
    int rr = (int)luaL_checkinteger(L, 2);
    int gg = (int)luaL_checkinteger(L, 3);
    int bb = (int)luaL_checkinteger(L, 4);

    int count = led_ctrl_get_led_count();
    if (index < 1 || index > count) {
        lua_pushfstring(L, "index must be 1..%d (got %d)", count, index);
        return lua_error(L);
    }
    if (rr < 0) { rr = 0; }
    if (rr > 255) { rr = 255; }
    if (gg < 0) { gg = 0; }
    if (gg > 255) { gg = 255; }
    if (bb < 0) { bb = 0; }
    if (bb > 255) { bb = 255; }

    esp_err_t err = led_ctrl_set_pixel(index - 1, (uint32_t)rr, (uint32_t)gg, (uint32_t)bb);
    lua_pushstring(L, err == ESP_OK ? "ok" : esp_err_to_name(err));
    return 1;
}

/* led.hsv(index, h, s, v)：单颗灯珠 HSV 色（h:0-360 色相, s/v:0-255）
 * 渐变彩虹/呼吸等效果的核心：色相连续变化即平滑渐变 */
static int lua_led_hsv(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    int s = (int)luaL_checkinteger(L, 3);
    int v = (int)luaL_checkinteger(L, 4);

    int count = led_ctrl_get_led_count();
    if (index < 1 || index > count) {
        lua_pushfstring(L, "index must be 1..%d (got %d)", count, index);
        return lua_error(L);
    }
    if (s < 0) { s = 0; }
    if (s > 255) { s = 255; }
    if (v < 0) { v = 0; }
    if (v > 255) { v = 255; }

    uint32_t r, g, b;
    hsv_to_rgb(h, s, v, &r, &g, &b);
    esp_err_t err = led_ctrl_set_pixel(index - 1, r, g, b);
    lua_pushstring(L, err == ESP_OK ? "ok" : esp_err_to_name(err));
    return 1;
}

/* led.show()：把缓冲里的像素一次性送到灯带（动画画完一帧后调用）*/
static int lua_led_show(lua_State *L)
{
    esp_err_t err = led_ctrl_refresh();
    lua_pushstring(L, err == ESP_OK ? "ok" : esp_err_to_name(err));
    return 1;
}

/* sleep(ms) */
static int lua_sleep(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    return 0;
}

/* ─── 公开 API ─── */

/* 建一个 Lua 状态并装好安全库 + 硬件绑定（主状态和后台状态共用）*/
static void lua_setup_state(lua_State *L, lua_CFunction print_fn)
{
    /* 只开安全库：base/table/string/math/utf8。不开 io/os/package/debug */
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 5);

    lua_pushcfunction(L, print_fn);
    lua_setglobal(L, "print");

    /* led 模块 */
    static const luaL_Reg led_lib[] = {
        { "set", lua_led_set },
        { "pixel", lua_led_pixel },
        { "rgb", lua_led_rgb },
        { "hsv", lua_led_hsv },
        { "show", lua_led_show },
        { NULL, NULL }
    };
    luaL_newlib(L, led_lib);
    lua_setglobal(L, "led");

    /* sleep */
    lua_pushcfunction(L, lua_sleep);
    lua_setglobal(L, "sleep");
}

esp_err_t lua_engine_init(void)
{
    if (s_L) {
        return ESP_OK;
    }
    s_L = lua_newstate_psram();
    if (!s_L) {
        ESP_LOGE(TAG, "lua_newstate_psram failed");
        return ESP_ERR_NO_MEM;
    }
    lua_setup_state(s_L, lua_capture_print);

    ESP_LOGI(TAG, "Lua engine initialized (safe libs + led binding)");
    return ESP_OK;
}

esp_err_t lua_engine_run(const char *script, char *out, size_t out_sz, uint32_t timeout_ms)
{
    if (!script || !out || out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_L) {
        return ESP_ERR_INVALID_STATE;  /* 未初始化 */
    }

    /* 互斥：一次性脚本先停掉后台效果（两者都控制灯带，不能同时跑）*/
    lua_stop_bg_wait();

    s_out = out;
    s_out_len = 0;
    s_out_cap = out_sz;
    out[0] = '\0';
    s_timeout_ms = timeout_ms;
    s_start_tick = xTaskGetTickCount();

    /* 指令计数钩子：每 1000 条指令检查一次超时 */
    lua_sethook(s_L, lua_timeout_hook, LUA_MASKCOUNT, 1000);
    int rc = luaL_dostring(s_L, script);
    lua_sethook(s_L, NULL, 0, 0);

    if (rc != LUA_OK) {
        const char *msg = lua_tostring(s_L, -1);
        if (!s_out_len) {
            capture_append("Lua error: ", 11);
        } else {
            capture_append("\nLua error: ", 12);
        }
        capture_append(msg ? msg : "unknown error", msg ? strlen(msg) : 13);
        lua_pop(s_L, 1);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ═══ 后台脚本（持续效果，esp-claw 简化版：一个后台槽，随时开始/停止）═══ */

#define BG_TASK_STACK  8192   /* 后台任务独立栈：跑 Lua + LED 驱动链 */
#define BG_TASK_PRIO   4      /* 低于对话/网络任务(5)：后台效果不能抢占 LLM 请求 */

static TaskHandle_t s_bg_task = NULL;
static volatile bool s_bg_stop = false;   /* 协作式停止标志 */
static char s_bg_name[24] = "";           /* 当前后台效果名（工具层查询用）*/

/* 后台脚本的 print：直接打日志（持续效果不需要回传文本）*/
static int lua_bg_print(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        ESP_LOGI(TAG, "[bg] %.*s", (int)len, s);
        lua_pop(L, 1);
    }
    return 0;
}

/* 后台停止钩子：每 1000 条指令检查一次停止标志（协作式取消）*/
static void lua_bg_stop_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (s_bg_stop) {
        luaL_error(L, "background script stopped");
    }
}

static void lua_bg_task(void *arg)
{
    char *script = (char *)arg;   /* strdup 的副本，用完释放 */
    lua_State *L = lua_newstate_psram();
    if (!L) {
        ESP_LOGE(TAG, "bg: lua_newstate_psram failed");
        free(script);
        s_bg_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    lua_setup_state(L, lua_bg_print);

    lua_sethook(L, lua_bg_stop_hook, LUA_MASKCOUNT, 1000);
    int rc = luaL_loadstring(L, script);
    free(script);
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, 0, 0);
    }
    if (rc != LUA_OK) {
        if (s_bg_stop) {
            /* 正常叫停路径：不是错误，别吓人 */
            ESP_LOGI(TAG, "bg script stopped by request");
        } else {
            ESP_LOGE(TAG, "bg script ended with error: %s",
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown");
        }
        lua_pop(L, 1);
    } else {
        ESP_LOGI(TAG, "bg script finished by itself");
    }
    lua_close(L);
    s_bg_name[0] = '\0';   /* 效果结束，清名 */
    s_bg_task = NULL;
    vTaskDelete(NULL);
}

bool lua_engine_bg_running(void)
{
    return s_bg_task != NULL;
}

const char *lua_engine_bg_name(void)
{
    return s_bg_name[0] ? s_bg_name : NULL;
}

void lua_engine_stop_bg(void)
{
    s_bg_stop = true;   /* 钩子会在 1000 条指令内中止脚本（含 sleep 醒来后）*/
}

/* 停掉后台脚本并等待退出（最多 3 秒）。
 * 互斥规则：一次只允许一个脚本控制灯带——新脚本顶掉旧脚本 */
static esp_err_t lua_stop_bg_wait(void)
{
    if (!s_bg_task) {
        return ESP_OK;
    }
    s_bg_stop = true;
    for (int i = 0; i < 30 && s_bg_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_bg_task) {
        ESP_LOGE(TAG, "bg script did not stop in time");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t lua_engine_start_bg(const char *name, const char *script)
{
    if (!script || script[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 互斥：先停掉旧的后台脚本 */
    esp_err_t err = lua_stop_bg_wait();
    if (err != ESP_OK) {
        return err;
    }

    s_bg_stop = false;
    if (name && name[0]) {
        strlcpy(s_bg_name, name, sizeof(s_bg_name));
    } else {
        s_bg_name[0] = '\0';
    }
    ESP_LOGI(TAG, "bg script [%s]:\n%s", s_bg_name[0] ? s_bg_name : "(anon)", script);
    char *copy = strdup(script);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(lua_bg_task, "lua_bg", BG_TASK_STACK, copy, BG_TASK_PRIO, &s_bg_task) != pdPASS) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "background script started");
    return ESP_OK;
}
