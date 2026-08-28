#include "stream_player.h"

#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"

#include "display_mgr.h"
#include "screen.h"
#include "serial_channel.h"
#include "serial_console.h"

static const char *TAG = "stream_player";

/* ========== 串流参数 ========== */
#define STREAM_BAUD       2000000    /* 串流波特率（PC 端 screen_stream.py 必须一致）*/
#define STREAM_IDLE_MS    2000       /* 超过 2 秒没收到数据 → 认为串流结束 */
#define STREAM_MAX_ERRORS 5          /* 连续坏帧 5 次 → 放弃本轮串流 */
#define MAX_JPEG_SIZE     (128 * 1024)  /* 单帧 JPEG 上限（PSRAM 暂存）*/

/* 帧协议（和 PC 端 screen_stream.py 一致）：
 *   [0xAA][0x55][长度 u32 大端][JPEG 数据...]
 *   带长度头的目的是：坏帧/丢字节后能重新同步 */
#define FRAME_MAGIC0      0xAA
#define FRAME_MAGIC1      0x55
#define FRAME_HEADER_LEN  6

static TaskHandle_t s_task = NULL;
static uint8_t *s_jpeg_buf = NULL;    /* PSRAM：暂存收到的 JPEG 帧 */
static uint8_t *s_fb = NULL;          /* 内部 RAM：RGB565 输出缓冲（SPI DMA 用）*/
static volatile bool s_active = false;

#define STREAM_UART ((uart_port_t)serial_console_get_uart())

bool stream_player_active(void)
{
    return s_active;
}

/* 从串口精确读取 len 字节；超时返回错误（串流结束）*/
static esp_err_t read_exact(uint8_t *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = uart_read_bytes(STREAM_UART, dst + got, len - got,
                                pdMS_TO_TICKS(STREAM_IDLE_MS));
        if (n <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        got += (size_t)n;
    }
    return ESP_OK;
}

/* 重新同步：逐字节扫描，寻找下一帧的魔数 AA 55。
 * RX 溢出丢过字节后，用这个恢复对齐。找到返回 true（魔数已消耗）。 */
static bool resync_frame(void)
{
    uint8_t b;
    for (int i = 0; i < 2048; i++) {
        if (read_exact(&b, 1) != ESP_OK) {
            return false;
        }
        if (b == FRAME_MAGIC0 && read_exact(&b, 1) == ESP_OK && b == FRAME_MAGIC1) {
            return true;
        }
    }
    return false;
}

/* ========== 串流任务 ==========
 * 挂起 → 被 display_mgr_show(DISPLAY_APP_STREAM) 恢复 → 开始一轮串流
 * 串流结束（超时/连续坏帧）→ 恢复串口控制台 → 自动切回 GIF */
static void stream_task(void *arg)
{
    while (1) {
        vTaskSuspend(NULL);   /* 等仲裁器激活 */

        /* 1. 接管串口：暂停控制台任务，切高速波特率，清掉残留数据 */
        s_active = true;
        serial_channel_suspend();
        uart_set_baudrate(STREAM_UART, STREAM_BAUD);
        uart_flush_input(STREAM_UART);
        ESP_LOGI(TAG, "stream session started (%d baud)", STREAM_BAUD);

        /* 2. 循环接收 + 解码 + 显示 */
        int errors = 0;
        while (errors < STREAM_MAX_ERRORS) {
            uint8_t hdr[FRAME_HEADER_LEN];

            /* 2.1 读帧头；超时说明 PC 停了 */
            if (read_exact(hdr, FRAME_HEADER_LEN) != ESP_OK) {
                ESP_LOGI(TAG, "no data for %d ms, stream idle", STREAM_IDLE_MS);
                break;
            }
            if (hdr[0] != FRAME_MAGIC0 || hdr[1] != FRAME_MAGIC1) {
                /* 帧头不对（可能 RX 溢出丢过字节）：重新同步 */
                ESP_LOGW(TAG, "bad frame magic, resyncing...");
                errors++;
                if (errors >= STREAM_MAX_ERRORS || !resync_frame()) {
                    break;
                }
                continue;
            }

            /* 2.2 帧体长度（大端 u32）*/
            uint32_t jlen = ((uint32_t)hdr[2] << 24) | ((uint32_t)hdr[3] << 16) |
                            ((uint32_t)hdr[4] << 8) | hdr[5];
            if (jlen == 0 || jlen > MAX_JPEG_SIZE) {
                ESP_LOGW(TAG, "bad frame length %u", jlen);
                errors++;
                continue;
            }
            if (read_exact(s_jpeg_buf, jlen) != ESP_OK) {
                break;   /* 帧体没收完 → 串流结束 */
            }

            /* 2.3 解码：esp_jpeg 直接输出 RGB565（小端，与屏幕一致）*/
            esp_jpeg_image_cfg_t cfg = {
                .indata = s_jpeg_buf,
                .indata_size = jlen,
                .outbuf = s_fb,
                .outbuf_size = SCREEN_WIDTH * SCREEN_HEIGHT * 2,
                .out_format = JPEG_IMAGE_FORMAT_RGB565,
                .out_scale = JPEG_IMAGE_SCALE_0,
            };
            esp_jpeg_image_output_t out;
            if (esp_jpeg_decode(&cfg, &out) != ESP_OK ||
                out.width != SCREEN_WIDTH || out.height != SCREEN_HEIGHT) {
                ESP_LOGW(TAG, "decode failed or wrong size (%ux%u)",
                         out.width, out.height);
                errors++;
                continue;
            }

            /* 2.4 上屏 */
            errors = 0;
            screen_show_bitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                               (const uint16_t *)s_fb);
        }

        /* 3. 串流结束：恢复控制台波特率与任务 */
        uart_flush_input(STREAM_UART);
        uart_set_baudrate(STREAM_UART, serial_console_get_baud());
        serial_channel_resume();
        s_active = false;
        ESP_LOGI(TAG, "stream session ended");

        /* 4. 回到默认画面（GIF）*/
        display_mgr_show(DISPLAY_APP_GIF);
    }
}

esp_err_t stream_player_init(void)
{
    if (s_task) {
        return ESP_OK;
    }

    /* 帧缓冲放 PSRAM：screen.c 已开 psram_dma_direct，SPI DMA 可直接读。
     * 之前放内部 RAM 导致 153KB 分配失败、QQ 发送 OOM（TLS 也要内部 RAM）。*/
    s_fb = heap_caps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 2,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "no PSRAM for frame buffer (%d bytes)",
                 SCREEN_WIDTH * SCREEN_HEIGHT * 2);
        return ESP_ERR_NO_MEM;
    }

    /* JPEG 暂存放 PSRAM（CPU 读，不需要 DMA）*/
    s_jpeg_buf = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_jpeg_buf) {
        ESP_LOGE(TAG, "no PSRAM for jpeg buffer (%d bytes)", MAX_JPEG_SIZE);
        free(s_fb);
        s_fb = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(stream_task, "stream_player", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        free(s_fb);
        free(s_jpeg_buf);
        s_fb = NULL;
        s_jpeg_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    display_mgr_register(DISPLAY_APP_STREAM, s_task);   /* 挂起并登记 */
    ESP_LOGI(TAG, "stream player registered");
    return ESP_OK;
}
