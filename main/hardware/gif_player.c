#include "gif_player.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_mgr.h"
#include "screen.h"

static const char *TAG = "gif_player";

/* ========== GIF 分区信息（和 tools/gif2rgb565.py 输出格式对应）========== */
#define GIF_PART_LABEL     "gif"   /* 分区表里的名字 */
#define GIF_PART_TYPE      ESP_PARTITION_TYPE_DATA
#define GIF_PART_SUBTYPE   0x82    /* 自定义数据子类型（0x80~0xFF 都行）*/

/* 帧数据文件头（12 字节）:
 *   [u32 帧数][u32 宽][u32 高]
 *   然后每帧: [u16 延时ms][RGB565 像素 宽*高*2 字节] */
typedef struct {
    uint32_t frame_count;
    uint32_t width;
    uint32_t height;
} gif_header_t;

#define HEADER_SIZE (sizeof(gif_header_t))

static const esp_partition_t *s_part;   /* 找到的 gif 分区 */
static gif_header_t s_hdr;              /* 文件头 */
static uint32_t s_frame_bytes;          /* 一帧像素的字节数 */
static uint8_t *s_fb;                   /* 帧缓冲 */

/* 读取分区里从 offset 开始的 len 字节到 dst（带日志的错误封装）*/
static esp_err_t gif_read(const esp_partition_t *part, uint32_t offset,
                          void *dst, size_t len)
{
    esp_err_t err = esp_partition_read(part, offset, dst, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read gif partition @0x%x failed: %s",
                 offset, esp_err_to_name(err));
    }
    return err;
}

esp_err_t gif_player_init(void)
{
    /* 1. 在分区表里找到 gif 数据分区 */
    s_part = esp_partition_find_first(GIF_PART_TYPE, GIF_PART_SUBTYPE,
                                      GIF_PART_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "gif partition not found! Flash yy_frames.bin first.");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "gif partition: offset 0x%x, size 0x%x (%u KB)",
             s_part->address, s_part->size, s_part->size / 1024);

    /* 2. 读文件头，校验数据合法性 */
    ESP_RETURN_ON_ERROR(gif_read(s_part, 0, &s_hdr, HEADER_SIZE),
                        TAG, "header read failed");
    if (s_hdr.frame_count == 0 || s_hdr.frame_count > 1000 ||
        s_hdr.width == 0 || s_hdr.height == 0 ||
        s_hdr.width > 240 || s_hdr.height > 320) {
        ESP_LOGE(TAG, "bad gif header: %u x %u, %u frames",
                 s_hdr.width, s_hdr.height, s_hdr.frame_count);
        return ESP_ERR_INVALID_ARG;
    }

    /* 3. 准备帧缓冲（必须用内部 RAM：SPI DMA 只能访问内部 SRAM）*/
    s_frame_bytes = s_hdr.width * s_hdr.height * 2;
    /* 帧缓冲放 PSRAM：screen.c 已开 psram_dma_direct，SPI DMA 可直接读。
     * 之前占内部 RAM（115KB），把内部 RAM 留给 TLS/网络。*/
    s_fb = heap_caps_malloc(s_frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "no PSRAM for frame buffer (%u bytes)", s_frame_bytes);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "gif ready: %u frames, %ux%u, %u bytes/frame",
             s_hdr.frame_count, s_hdr.width, s_hdr.height, s_frame_bytes);
    return ESP_OK;
}

/* 播放任务：循环读帧 -> 画帧 -> 等延时 */
static TaskHandle_t s_task = NULL;

static void gif_player_task(void *arg)
{
    /* 注册后先挂起：是否播放由 display_mgr_show() 决定 */
    vTaskSuspend(NULL);

    uint32_t offset = HEADER_SIZE;   /* 文件头之后是第一帧的延时 */

    while (1) {
        for (uint32_t i = 0; i < s_hdr.frame_count; i++) {
            uint16_t delay_ms = 0;

            /* 从 Flash 读这一帧的延时和像素数据 */
            if (gif_read(s_part, offset, &delay_ms, sizeof(delay_ms)) != ESP_OK ||
                gif_read(s_part, offset + sizeof(delay_ms), s_fb, s_frame_bytes) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            offset += sizeof(delay_ms) + s_frame_bytes;

            /* 画到屏幕（240x240 动画在 320x240 横屏上居中显示）*/
            int x = ((int)SCREEN_WIDTH - (int)s_hdr.width) / 2;
            int y = ((int)SCREEN_HEIGHT - (int)s_hdr.height) / 2;
            screen_show_bitmap(x, y, (int)s_hdr.width, (int)s_hdr.height,
                               (const uint16_t *)s_fb);

            /* 按 GIF 原节奏等待（延时 30ms，绘制约 23ms，节奏基本一致）*/
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        offset = HEADER_SIZE;   /* 播完一圈，回到第一帧（循环播放）*/
    }
}

void gif_player_register(void)
{
    if (s_task) {
        return;
    }
    if (!s_part || !s_fb) {
        ESP_LOGE(TAG, "gif not initialized, call gif_player_init() first");
        return;
    }
    if (xTaskCreate(gif_player_task, "gif_player", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return;
    }
    display_mgr_register(DISPLAY_APP_GIF, s_task);   /* 挂起并登记，等待仲裁 */
    ESP_LOGI(TAG, "gif player registered");
}
