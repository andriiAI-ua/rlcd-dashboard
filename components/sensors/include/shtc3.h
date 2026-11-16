// SHTC3 temperature and humidity sensor (I2C address 0x70).
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Add the sensor to the bus and read its ID.
esp_err_t shtc3_init(i2c_master_bus_handle_t bus);

// Measure the temperature (degrees Celsius) and the relative humidity (percent).
// The sensor sleeps between measurements.
esp_err_t shtc3_read(float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif
