// Driver for the ST7305 reflective LCD on the Waveshare ESP32-S3-RLCD-4.2 (400 x 300, 1 bit).
// The init sequence and the pixel layout come from the Waveshare examples (Apache-2.0).
#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7305_WIDTH  400
#define ST7305_HEIGHT 300

// Bytes in a canvas: one bit for each pixel, rows from top to bottom.
#define ST7305_CANVAS_BYTES (ST7305_WIDTH / 8 * ST7305_HEIGHT)

typedef struct {
    spi_host_device_t host;
    gpio_num_t sck;
    gpio_num_t mosi;
    gpio_num_t cs;
    gpio_num_t dc;
    gpio_num_t rst;
    int clock_hz;
} st7305_config_t;

// Start the SPI bus, reset the panel and send the init sequence.
esp_err_t st7305_init(const st7305_config_t *config);

// Show a canvas on the panel.
// The canvas has 400 x 300 pixels, 50 bytes for each row, the most significant bit first.
// A bit set to 1 is a black pixel.
esp_err_t st7305_show(const uint8_t *canvas);

#ifdef __cplusplus
}
#endif
