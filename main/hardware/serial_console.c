#include "serial_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "serial_console";

#define SERIAL_BUF_SIZE    256
#define SERIAL_RX_BUF_SIZE (1024 * 16)  /* 16KB：串流模式（2M 波特率）下防解码期间 RX 溢出 */

static uart_port_t s_uart_num = UART_NUM_0;
static int s_baud = 115200;             /* 当前波特率（串流结束后要恢复它）*/
static SemaphoreHandle_t s_printf_mutex;  /* 保护 print_buf：多个任务并发 printf 会互相覆盖（曾导致中文乱码）*/

esp_err_t serial_console_init(const serial_console_config_t *config)
{
    /* 默认走 UART0 (GPIO43/44)，即板载 USB-UART 芯片(CH343)接的通道，
     * 与系统日志共用同一个串口，无需外接 USB-TTL。 */
    int rx_pin = (config) ? config->rx_pin : 44;
    int tx_pin = (config) ? config->tx_pin : 43;
    int baud_rate = (config) ? config->baud_rate : 115200;
    s_uart_num = (config) ? (uart_port_t)config->uart_num : UART_NUM_0;
    s_baud = baud_rate;

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(s_uart_num, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = uart_set_pin(s_uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = uart_driver_install(s_uart_num, SERIAL_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 旧版 IDF：UART0 已由系统控制台安装时返回此值，直接复用即可 */
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    s_printf_mutex = xSemaphoreCreateMutex();
    if (!s_printf_mutex) {
        ESP_LOGE(TAG, "Failed to create printf mutex");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Serial console initialized: UART%d, baud=%d, rx=GPIO%d, tx=GPIO%d",
             s_uart_num, baud_rate, rx_pin, tx_pin);
    return ESP_OK;
}

esp_err_t serial_console_readline(char *buffer, size_t buf_size, uint32_t timeout_ms)
{
    if (!buffer || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int offset = 0;
    TickType_t timeout_tick = (timeout_ms > 0) ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        int rx_len = uart_read_bytes(s_uart_num, (uint8_t *)&buffer[offset], 1, timeout_tick);
        if (rx_len <= 0) {
            if (rx_len < 0) {
                return (esp_err_t)rx_len;
            }
            if (offset == 0) {
                return ESP_ERR_TIMEOUT;
            }
            /* 超时但已有部分输入：补终止符，按完整一行返回 */
            buffer[offset] = '\0';
            break;
        }

        if (buffer[offset] == '\n' || buffer[offset] == '\r') {
            buffer[offset] = '\0';
            offset++;
            break;
        }

        offset++;
        if (offset >= (int)(buf_size - 1)) {
            buffer[offset] = '\0';
            break;
        }

        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (timeout_ms > 0 && pdTRUE == (elapsed >= pdMS_TO_TICKS(timeout_ms))) {
            if (offset == 0) {
                return ESP_ERR_TIMEOUT;
            }
            buffer[offset] = '\0';
            break;
        }
    }

    return ESP_OK;
}

int serial_console_write(const char *data, size_t len)
{
    if (!data || len == 0) {
        return 0;
    }

    return uart_write_bytes(s_uart_num, data, len);
}

int serial_console_printf(const char *format, ...)
{
    static char print_buf[SERIAL_BUF_SIZE];
    va_list args;
    int len = 0;

    /* 加锁保证"格式化+发送"原子完成：多个任务并发 printf 会互相覆盖静态缓冲，
     * 造成字节穿插（曾出现 "S> ing to LLM..." 和中文乱码）*/
    if (s_printf_mutex) {
        xSemaphoreTake(s_printf_mutex, portMAX_DELAY);
    }

    va_start(args, format);
    len = vsnprintf(print_buf, sizeof(print_buf), format, args);
    va_end(args);

    if (len > 0) {
        len = uart_write_bytes(s_uart_num, print_buf, len);
    }

    if (s_printf_mutex) {
        xSemaphoreGive(s_printf_mutex);
    }
    return len;
}

void serial_console_deinit(void)
{
    uart_driver_delete(s_uart_num);
    ESP_LOGI(TAG, "Serial console deinitialized");
}

/* 供 stream_player 使用：读写同一个 UART / 恢复原波特率 */
int serial_console_get_uart(void)
{
    return (int)s_uart_num;
}

int serial_console_get_baud(void)
{
    return s_baud;
}