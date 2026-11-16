// SHTC3 driver. The commands and the CRC come from the Sensirion SHTC3 datasheet.
#include "shtc3.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3";

#define SHTC3_ADDRESS     0x70
#define CMD_WAKEUP        0x3517
#define CMD_SLEEP         0xB098
#define CMD_SOFT_RESET    0x805D
#define CMD_READ_ID       0xEFC8
// Normal mode, temperature first, no clock stretching
#define CMD_MEASURE       0x7866
#define TIMEOUT_MS        100

static i2c_master_dev_handle_t s_dev;

static esp_err_t write_command(uint16_t cmd)
{
    const uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), TIMEOUT_MS);
}

static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t wakeup(void)
{
    ESP_RETURN_ON_ERROR(write_command(CMD_WAKEUP), TAG, "wakeup");
    // The datasheet gives 240 us maximum for the wakeup.
    vTaskDelay(pdMS_TO_TICKS(1));
    return ESP_OK;
}

esp_err_t shtc3_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");
    ESP_RETURN_ON_ERROR(wakeup(), TAG, "first wakeup");
    ESP_RETURN_ON_ERROR(write_command(CMD_SOFT_RESET), TAG, "soft reset");
    vTaskDelay(pdMS_TO_TICKS(1));

    const uint8_t cmd[2] = { CMD_READ_ID >> 8, CMD_READ_ID & 0xFF };
    uint8_t id[3];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, cmd, sizeof(cmd), id, sizeof(id), TIMEOUT_MS),
                        TAG, "read ID");
    ESP_RETURN_ON_FALSE(crc8(id, 2) == id[2], ESP_ERR_INVALID_CRC, TAG, "ID CRC");
    ESP_LOGI(TAG, "SHTC3 ID 0x%04x", (id[0] << 8) | id[1]);
    return write_command(CMD_SLEEP);
}

esp_err_t shtc3_read(float *temperature, float *humidity)
{
    ESP_RETURN_ON_ERROR(wakeup(), TAG, "wakeup");
    esp_err_t err = write_command(CMD_MEASURE);
    uint8_t data[6] = { 0 };
    if (err == ESP_OK) {
        // The datasheet gives 12.1 ms maximum for a measurement in normal mode.
        vTaskDelay(pdMS_TO_TICKS(15));
        err = i2c_master_receive(s_dev, data, sizeof(data), TIMEOUT_MS);
    }
    write_command(CMD_SLEEP);
    ESP_RETURN_ON_ERROR(err, TAG, "measure");
    ESP_RETURN_ON_FALSE(crc8(data, 2) == data[2] && crc8(data + 3, 2) == data[5],
                        ESP_ERR_INVALID_CRC, TAG, "measurement CRC");

    const uint16_t raw_t = (uint16_t)((data[0] << 8) | data[1]);
    const uint16_t raw_h = (uint16_t)((data[3] << 8) | data[4]);
    *temperature = -45.0f + 175.0f * raw_t / 65536.0f;
    *humidity = 100.0f * raw_h / 65536.0f;
    return ESP_OK;
}
