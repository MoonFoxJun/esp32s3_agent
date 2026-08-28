#include "display_mgr.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "display_mgr";

static TaskHandle_t s_tasks[DISPLAY_APP_MAX];   /* 每个应用的任务句柄 */
static display_app_t s_current = DISPLAY_APP_NONE;

esp_err_t display_mgr_init(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));
    s_current = DISPLAY_APP_NONE;
    ESP_LOGI(TAG, "display manager ready");
    return ESP_OK;
}

esp_err_t display_mgr_register(display_app_t app, TaskHandle_t task)
{
    if (app <= DISPLAY_APP_NONE || app >= DISPLAY_APP_MAX || !task) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tasks[app] = task;
    vTaskSuspend(task);              /* 注册后先挂起，等 show() 激活 */
    ESP_LOGI(TAG, "app %d registered", (int)app);
    return ESP_OK;
}

esp_err_t display_mgr_show(display_app_t app)
{
    if (app < DISPLAY_APP_NONE || app >= DISPLAY_APP_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app == s_current) {
        return ESP_OK;               /* 已经是这个应用，不用切 */
    }

    display_app_t old = s_current;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    /* 先记录新状态，再动手挂起/恢复：
     * 如果调用者就是当前应用自己（比如串流结束自动切回 GIF），
     * 不能挂起自己，否则后面的代码永远执行不到。 */
    s_current = app;

    if (old != DISPLAY_APP_NONE && s_tasks[old] && s_tasks[old] != self) {
        vTaskSuspend(s_tasks[old]);  /* 挂起旧应用 */
    }
    if (app != DISPLAY_APP_NONE && s_tasks[app]) {
        vTaskResume(s_tasks[app]);   /* 恢复新应用 */
    }

    ESP_LOGI(TAG, "switch: app %d -> %d", (int)old, (int)app);
    return ESP_OK;
}

display_app_t display_mgr_current(void)
{
    return s_current;
}
