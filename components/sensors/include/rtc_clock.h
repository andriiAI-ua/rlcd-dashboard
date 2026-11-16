// Hardware clock: a DS3231 module on the header (0x68) if it is there, else the PCF85063 on the board (0x51).
// The clock keeps UTC time.
#pragma once

#include <time.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_CLOCK_NONE = 0,
    RTC_CLOCK_PCF85063,
    RTC_CLOCK_DS3231,
} rtc_clock_kind_t;

// Find the clock chip on the bus. A DS3231 module has priority over the PCF85063.
esp_err_t rtc_clock_init(i2c_master_bus_handle_t bus);

rtc_clock_kind_t rtc_clock_kind(void);

// Return the chip name, for example "DS3231", or "none".
const char *rtc_clock_name(void);

// Read the UTC time. Return ESP_ERR_INVALID_STATE if the clock lost the time
// (no backup battery, or the time was never set).
esp_err_t rtc_clock_read(time_t *utc);

// Write the UTC time.
esp_err_t rtc_clock_write(time_t utc);

#ifdef __cplusplus
}
#endif
