#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 屏幕显示仲裁器（display manager）
 *
 * 屏幕是共享资源，同一时刻只允许一个"应用"使用：
 *   - GIF 播放器
 *   - 串口串流（PC 画面）
 *   - 空闲（黑屏）
 *
 * 每个应用是一个 FreeRTOS 任务，注册时处于挂起状态；
 * display_mgr_show() 负责切换：挂起旧应用任务、恢复新应用任务。
 * 用软件仲裁代替 GPIO 硬切换：任何来源（串口命令 / LLM 工具调用）
 * 都能申请屏幕，且无需额外接线。
 */

typedef enum {
    DISPLAY_APP_NONE = 0,   /* 空闲：无应用占用屏幕 */
    DISPLAY_APP_GIF,        /* GIF 动画播放器 */
    DISPLAY_APP_STREAM,     /* 串口串流（PC 画面） */
    DISPLAY_APP_MAX,
} display_app_t;

/* 初始化仲裁器（开机调用一次）*/
esp_err_t display_mgr_init(void);

/* 注册一个应用：把它的任务登记进来并挂起（开机调用一次）*/
esp_err_t display_mgr_register(display_app_t app, TaskHandle_t task);

/* 切换到指定应用：挂起旧任务，恢复新任务（可随时调用）*/
esp_err_t display_mgr_show(display_app_t app);

/* 当前占用屏幕的应用 */
display_app_t display_mgr_current(void);

#ifdef __cplusplus
}
#endif
