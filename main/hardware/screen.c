#include "screen.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "screen";

/* ========== 引脚配置（用户接线）========== */
#define PIN_SCLK   4   /* SPI 时钟 */
#define PIN_MOSI   5   /* SPI 数据 */
#define PIN_RES    6   /* 复位 */
#define PIN_DC     7   /* 数据/命令选择 */
#define PIN_CS     15  /* 片选 */
#define PIN_BL     16  /* 背光 */

#define LCD_HOST   SPI2_HOST
#define LCD_PCLK_HZ (40 * 1000 * 1000)   /* SPI 时钟 40MHz（ST7789 标准支持，若画面花屏可降回 20MHz）*/

static esp_lcd_panel_handle_t s_panel;

esp_err_t screen_init(void)
{
    if (s_panel) {
        return ESP_OK;
    }

    /* 1. 背光引脚设为输出并点亮 */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_BL, 1);

    /* 2. 初始化 SPI 总线（SCLK/MOSI）*/
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,              /* 只写，不读 */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCREEN_WIDTH * SCREEN_HEIGHT * 2 + 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    /* 3. 创建 SPI 面板 IO（CS/DC 在这里配置）
     *    psram_dma_direct：允许 SPI DMA 直接读 PSRAM 里的帧缓冲，
     *    这样大帧缓冲可以放 PSRAM，省下宝贵的内部 RAM（QQ/TLS 也要用）。*/
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.psram_dma_direct = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io),
                        TAG, "Panel IO init failed");

    /* 4. 创建 ST7789 面板（IDF 6.0：颜色顺序用 rgb_ele_order）
     *    字节序说明：SPI 把内存里的 uint16_t 原样发出（小端：低字节在前），
     *    而 ST7789 默认按“先收的字节是高字节”解释（大端），于是每个像素的
     *    高低字节对调——蓝色 0x001F 会被读成 0x1F00（绿色）。
     *    所以必须显式指定小端，让面板按“先收的字节是低字节”解释。 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,           /* RGB565 */
        .reset_gpio_num = PIN_RES,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel),
                        TAG, "ST7789 panel init failed");

    /* 5. 初始化面板 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_lcd_panel_invert_color(s_panel, true);   /* ST7789 一般需要反转颜色 */
    esp_lcd_panel_swap_xy(s_panel, true);        /* 旋转 90°：横屏 320x240（MADCTL 的 MV 位）*/
    esp_lcd_panel_mirror(s_panel, false, true);  /* 镜像 Y：让画面正立（若颠倒/镜像，改这两个参数）*/
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "ST7789 %dx%d initialized (SPI %dMHz)",
             SCREEN_WIDTH, SCREEN_HEIGHT, LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

esp_err_t screen_fill(uint16_t color)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 全屏填充：逐行画 */
    uint16_t *row = malloc(SCREEN_WIDTH * sizeof(uint16_t));
    if (!row) {
        return ESP_ERR_NO_MEM;
    }
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        row[x] = color;
    }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, SCREEN_WIDTH, y + 1, row);
    }
    free(row);
    return ESP_OK;
}

esp_err_t screen_show_bitmap(int x, int y, int w, int h, const uint16_t *rgb565)
{
    if (!s_panel || !rgb565 || x < 0 || y < 0 ||
        x + w > SCREEN_WIDTH || y + h > SCREEN_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, rgb565);
}

void screen_backlight(bool on)
{
    gpio_set_level(PIN_BL, on ? 1 : 0);
}
