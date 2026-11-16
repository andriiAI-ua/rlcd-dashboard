// ST7305 reflective LCD driver: SPI commands and the conversion of a canvas to the panel layout.
#include "st7305.h"

#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7305";

// The panel memory has the same number of bytes as the canvas.
#define FRAME_BYTES ST7305_CANVAS_BYTES

// In landscape mode one panel byte holds 2 columns x 4 rows.
#define ROW_BLOCKS (ST7305_HEIGHT / 4)

static spi_device_handle_t s_dev;
static gpio_num_t s_dc;
static uint8_t *s_frame;

static esp_err_t send(bool data, const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    gpio_set_level(s_dc, data ? 1 : 0);
    spi_transaction_t t = {
        .length = len * 8,
    };
    if (len <= sizeof(t.tx_data)) {
        // Short transfers go in the transaction itself, without DMA.
        memcpy(t.tx_data, buf, len);
        t.flags = SPI_TRANS_USE_TXDATA;
    } else {
        // The SPI driver copies a buffer that DMA cannot read (for example, in flash).
        t.tx_buffer = buf;
    }
    return spi_device_transmit(s_dev, &t);
}

static esp_err_t command(uint8_t cmd, const uint8_t *params, size_t count)
{
    ESP_RETURN_ON_ERROR(send(false, &cmd, 1), TAG, "command 0x%02x", cmd);
    return send(true, params, count);
}

#define CMD(cmd, ...)                                                       \
    do {                                                                    \
        static const uint8_t p_[] = { __VA_ARGS__ };                        \
        ESP_RETURN_ON_ERROR(command((cmd), p_, sizeof(p_)), TAG, "init");   \
    } while (0)

#define CMD0(cmd) ESP_RETURN_ON_ERROR(command((cmd), NULL, 0), TAG, "init")

static esp_err_t send_init_sequence(void)
{
    // The values are the same as in the Waveshare examples.
    CMD(0xD6, 0x17, 0x02);                     // NVM load control
    CMD(0xD1, 0x01);                           // Booster enable
    CMD(0xC0, 0x11, 0x04);                     // Gate voltage control
    CMD(0xC1, 0x69, 0x69, 0x69, 0x69);         // VSHP setting
    CMD(0xC2, 0x19, 0x19, 0x19, 0x19);         // Source voltage (vendor value)
    CMD(0xC4, 0x4B, 0x4B, 0x4B, 0x4B);         // Source voltage (vendor value)
    CMD(0xC5, 0x19, 0x19, 0x19, 0x19);         // Source voltage (vendor value)
    CMD(0xD8, 0x80, 0xE9);                     // Vendor value
    CMD(0xB2, 0x02);                           // Frame rate (vendor value)
    CMD(0xB3, 0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45); // Gate timing (vendor value)
    CMD(0xB4, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45);             // Gate timing (vendor value)
    CMD(0x62, 0x32, 0x03, 0x1F);               // Vendor value
    CMD(0xB7, 0x13);                           // Vendor value
    CMD(0xB0, 0x64);                           // Vendor value
    CMD0(0x11);                                // Sleep out
    vTaskDelay(pdMS_TO_TICKS(200));
    CMD(0xC9, 0x00);                           // Vendor value
    CMD(0x36, 0x48);                           // Memory data access control
    CMD(0x3A, 0x11);                           // Data format
    CMD(0xB9, 0x20);                           // Vendor value
    CMD(0xB8, 0x29);                           // Vendor value
    CMD0(0x21);                                // Display inversion on
    CMD(0x2A, 0x12, 0x2A);                     // Column address
    CMD(0x2B, 0x00, 0xC7);                     // Row address
    CMD(0x35, 0x00);                           // Tearing effect line on
    CMD(0xD0, 0xFF);                           // Vendor value
    CMD0(0x38);                                // Vendor value
    CMD0(0x29);                                // Display on
    return ESP_OK;
}

static void hard_reset(gpio_num_t rst)
{
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t st7305_init(const st7305_config_t *config)
{
    s_dc = config->dc;

    s_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_frame, ESP_ERR_NO_MEM, TAG, "frame buffer");

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << config->dc) | (1ULL << config->rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "DC and RST pins");

    spi_bus_config_t bus = {
        .mosi_io_num = config->mosi,
        .miso_io_num = -1,
        .sclk_io_num = config->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->host, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = config->clock_hz,
        .mode = 0,
        .spics_io_num = config->cs,
        .queue_size = 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(config->host, &dev, &s_dev), TAG, "SPI device");

    // In light sleep ESP-IDF isolates all pins (ESP_SLEEP_GPIO_RESET_WORKAROUND). Then RST floats,
    // the panel resets and stays empty after the wakeup. Keep the panel pins as they are in sleep.
    const gpio_num_t keep[] = { config->rst, config->dc, config->cs, config->sck, config->mosi };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(keep[i]), TAG, "sleep config of GPIO %d", keep[i]);
    }

    hard_reset(config->rst);
    ESP_RETURN_ON_ERROR(send_init_sequence(), TAG, "init sequence");
    ESP_LOGI(TAG, "Panel is ready, SPI clock %d Hz", config->clock_hz);
    return ESP_OK;
}

// Convert a canvas (rows, 1 = black) to the panel layout (1 = white).
// Panel byte index: (x / 2) * 75 + (299 - y) / 4.
// Bit in the byte: 7 - (((299 - y) % 4) * 2 + x % 2).
static void convert(const uint8_t *canvas, uint8_t *frame)
{
    const int row_bytes = ST7305_WIDTH / 8;
    uint8_t *out = frame;
    for (int x = 0; x < ST7305_WIDTH; x += 2) {
        const int byte_col = x >> 3;
        const int shift0 = 7 - (x & 7);
        const int shift1 = shift0 - 1;
        for (int block = 0; block < ROW_BLOCKS; block++) {
            uint8_t value = 0;
            for (int k = 0; k < 4; k++) {
                const int y = ST7305_HEIGHT - 1 - (block * 4 + k);
                const uint8_t src = canvas[y * row_bytes + byte_col];
                const int white0 = !((src >> shift0) & 1);
                const int white1 = !((src >> shift1) & 1);
                value |= (uint8_t)(white0 << (7 - k * 2));
                value |= (uint8_t)(white1 << (6 - k * 2));
            }
            *out++ = value;
        }
    }
}

esp_err_t st7305_show(const uint8_t *canvas)
{
    convert(canvas, s_frame);
    CMD(0x2A, 0x12, 0x2A);
    CMD(0x2B, 0x00, 0xC7);
    ESP_RETURN_ON_ERROR(command(0x2C, s_frame, FRAME_BYTES), TAG, "memory write");
    return ESP_OK;
}
