#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 串口控制台配置
 */
typedef struct {
    int rx_pin;           /* 接收引脚 (默认 GPIO44, UART0) */
    int tx_pin;           /* 发送引脚 (默认 GPIO43, UART0) */
    int baud_rate;        /* 波特率 (默认 115200) */
    int uart_num;         /* UART 编号 (默认 UART_NUM_0, 与板载 USB-UART 芯片共用) */
} serial_console_config_t;

/**
 * 初始化串口控制台
 *
 * @param config 串口配置 (NULL 则使用默认配置)
 * @return ESP_OK 成功
 */
esp_err_t serial_console_init(const serial_console_config_t *config);

/**
 * 等待并读取一行串口输入
 *
 * @param buffer   输出缓冲区
 * @param buf_size 缓冲区大小
 * @param timeout_ms 超时时间 (毫秒)，0 表示无限等待
 * @return ESP_OK 成功读取一行, ESP_ERR_TIMEOUT 超时
 */
esp_err_t serial_console_readline(char *buffer, size_t buf_size, uint32_t timeout_ms);

/**
 * 串口发送数据
 *
 * @param data 数据指针
 * @param len  数据长度
 * @return 实际发送的字节数
 */
int serial_console_write(const char *data, size_t len);

/**
 * 串口发送格式化字符串 (类似 printf)
 */
int serial_console_printf(const char *format, ...);

/**
 * 反初始化串口控制台，释放资源
 */
void serial_console_deinit(void);

/**
 * 获取当前 UART 编号与波特率（供串流模块切换/恢复波特率）
 */
int serial_console_get_uart(void);
int serial_console_get_baud(void);

#ifdef __cplusplus
}
#endif