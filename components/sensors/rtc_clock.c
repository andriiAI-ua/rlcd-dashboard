// Hardware clock drivers: DS3231 (datasheet registers 0x00-0x0F) and PCF85063A (registers 0x00-0x0A).
#include "rtc_clock.h"

#include <stdbool.h>
#include <sys/time.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_util.h"

static const char *TAG = "rtc";

#define DS3231_ADDRESS    0x68
#define PCF85063_ADDRESS  0x51
#define TIMEOUT_MS        100
#define PROBE_TIMEOUT_MS  50

// A clock with a year before this one did not keep the time.
#define MIN_VALID_YEAR    2024

// rtc_clock_read_edge(): the longest wait for the next chip second.
#define EDGE_TIMEOUT_US   1200000
// PCF85063A: after STOP is cleared, the first second ends 0.507813 to 0.507935 s later (the prescaler
// stages F0 and F1 are not reset).
#define PCF85063_START_US 507874
// The time for the I2C writes before the chip starts.
#define WRITE_MARGIN_US   20000

static rtc_clock_kind_t s_kind = RTC_CLOCK_NONE;
static i2c_master_dev_handle_t s_dev;

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin_to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static time_t to_epoch(int year, int month, int day, int hour, int minute, int second)
{
    return time_utc(year, month, day, hour, minute, second);
}

// Return the system time in microseconds since 1970.
static int64_t system_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Wait until the system time is at target_us. A task delay gives most of the wait, a short busy wait the end.
static void wait_until(int64_t target_us)
{
    const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
    for (;;) {
        const int64_t left = target_us - system_us();
        if (left <= 0) {
            return;
        }
        if (left > 3 * tick_us) {
            // A delay of n ticks is n - 1 to n ticks long: it stops before the target.
            vTaskDelay((TickType_t)(left / tick_us - 2));
        } else {
            esp_rom_delay_us((uint32_t)left);
        }
    }
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, TIMEOUT_MS);
}

static esp_err_t write_regs(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t data[9];
    if (len + 1 > sizeof(data)) {
        return ESP_ERR_INVALID_SIZE;
    }
    data[0] = reg;
    for (size_t i = 0; i < len; i++) {
        data[i + 1] = buf[i];
    }
    return i2c_master_transmit(s_dev, data, len + 1, TIMEOUT_MS);
}

static esp_err_t add_device(i2c_master_bus_handle_t bus, uint8_t address)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

/* DS3231 */

static esp_err_t ds3231_setup(void)
{
    // Status register: clear EN32kHz (bit 3). The 32 kHz output is not used.
    uint8_t status;
    ESP_RETURN_ON_ERROR(read_regs(0x0F, &status, 1), TAG, "DS3231 status");
    status &= (uint8_t)~0x08;
    ESP_RETURN_ON_ERROR(write_regs(0x0F, &status, 1), TAG, "DS3231 status");
    // Control register: clear EOSC (bit 7), so that the oscillator runs on the battery.
    uint8_t control;
    ESP_RETURN_ON_ERROR(read_regs(0x0E, &control, 1), TAG, "DS3231 control");
    if (control & 0x80) {
        control &= (uint8_t)~0x80;
        ESP_RETURN_ON_ERROR(write_regs(0x0E, &control, 1), TAG, "DS3231 control");
    }
    return ESP_OK;
}

static esp_err_t ds3231_read(time_t *utc)
{
    uint8_t status;
    ESP_RETURN_ON_ERROR(read_regs(0x0F, &status, 1), TAG, "DS3231 status");
    uint8_t r[7];
    ESP_RETURN_ON_ERROR(read_regs(0x00, r, sizeof(r)), TAG, "DS3231 time");

    int hour;
    if (r[2] & 0x40) {
        // 12-hour mode: bit 5 is PM.
        hour = bcd_to_bin(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0);
    } else {
        hour = bcd_to_bin(r[2] & 0x3F);
    }
    const int year = 2000 + bcd_to_bin(r[6]);
    if ((status & 0x80) || year < MIN_VALID_YEAR) {
        // OSF (bit 7): the oscillator stopped, so the time is not correct.
        return ESP_ERR_INVALID_STATE;
    }
    *utc = to_epoch(year, bcd_to_bin(r[5] & 0x1F), bcd_to_bin(r[4] & 0x3F), hour,
                    bcd_to_bin(r[1] & 0x7F), bcd_to_bin(r[0] & 0x7F));
    return ESP_OK;
}

static esp_err_t ds3231_write(const struct tm *t)
{
    const uint8_t r[7] = {
        bin_to_bcd(t->tm_sec),
        bin_to_bcd(t->tm_min),
        bin_to_bcd(t->tm_hour),          // bit 6 = 0: 24-hour mode
        (uint8_t)(t->tm_wday + 1),       // 1..7
        bin_to_bcd(t->tm_mday),
        bin_to_bcd(t->tm_mon + 1),
        bin_to_bcd(t->tm_year % 100),
    };
    ESP_RETURN_ON_ERROR(write_regs(0x00, r, sizeof(r)), TAG, "DS3231 time");
    uint8_t status;
    ESP_RETURN_ON_ERROR(read_regs(0x0F, &status, 1), TAG, "DS3231 status");
    status &= (uint8_t)~0x80;  // clear OSF
    return write_regs(0x0F, &status, 1);
}

// The DS3231 resets its countdown chain when the seconds register is written: write the time at the start
// of the next system second.
static esp_err_t ds3231_write_now(void)
{
    const int64_t start_us = (system_us() / 1000000 + 1) * 1000000;
    const time_t utc = (time_t)(start_us / 1000000);
    struct tm t;
    gmtime_r(&utc, &t);
    wait_until(start_us);
    return ds3231_write(&t);
}

/* PCF85063A */

static esp_err_t pcf85063_setup(void)
{
    // Control_1: clear STOP (bit 5) and 12_24 (bit 1): the clock runs in 24-hour mode.
    uint8_t ctrl;
    ESP_RETURN_ON_ERROR(read_regs(0x00, &ctrl, 1), TAG, "PCF85063 control");
    if (ctrl & 0x22) {
        ctrl &= (uint8_t)~0x22;
        ESP_RETURN_ON_ERROR(write_regs(0x00, &ctrl, 1), TAG, "PCF85063 control");
    }
    return ESP_OK;
}

static esp_err_t pcf85063_read(time_t *utc)
{
    uint8_t r[7];
    ESP_RETURN_ON_ERROR(read_regs(0x04, r, sizeof(r)), TAG, "PCF85063 time");
    const int year = 2000 + bcd_to_bin(r[6]);
    if ((r[0] & 0x80) || year < MIN_VALID_YEAR) {
        // OS (bit 7 of Seconds): the oscillator stopped, so the time is not correct.
        return ESP_ERR_INVALID_STATE;
    }
    *utc = to_epoch(year, bcd_to_bin(r[5] & 0x1F), bcd_to_bin(r[3] & 0x3F), bcd_to_bin(r[2] & 0x3F),
                    bcd_to_bin(r[1] & 0x7F), bcd_to_bin(r[0] & 0x7F));
    return ESP_OK;
}

static esp_err_t pcf85063_write(const struct tm *t)
{
    const uint8_t r[7] = {
        bin_to_bcd(t->tm_sec),           // bit 7 = 0 clears OS
        bin_to_bcd(t->tm_min),
        bin_to_bcd(t->tm_hour),
        bin_to_bcd(t->tm_mday),
        (uint8_t)t->tm_wday,             // 0..6, Sunday = 0
        bin_to_bcd(t->tm_mon + 1),
        bin_to_bcd(t->tm_year % 100),
    };
    return write_regs(0x04, r, sizeof(r));
}

// STOP (bit 5 of Control_1) holds the prescaler in reset. Set STOP, write the time of the current second,
// and clear STOP PCF85063_START_US before the next system second: then the chip second ends with it.
static esp_err_t pcf85063_write_now(void)
{
    uint8_t ctrl;
    ESP_RETURN_ON_ERROR(read_regs(0x00, &ctrl, 1), TAG, "PCF85063 control");
    int64_t start_us = (system_us() / 1000000 + 1) * 1000000;
    if (start_us - PCF85063_START_US - system_us() < WRITE_MARGIN_US) {
        start_us += 1000000;
    }
    const time_t utc = (time_t)(start_us / 1000000 - 1);
    struct tm t;
    gmtime_r(&utc, &t);
    const uint8_t stop = ctrl | 0x20;
    ESP_RETURN_ON_ERROR(write_regs(0x00, &stop, 1), TAG, "PCF85063 stop");
    const esp_err_t err = pcf85063_write(&t);
    wait_until(start_us - PCF85063_START_US);
    // Clear STOP also after an error: the clock must run.
    const uint8_t run = ctrl & (uint8_t)~0x20;
    const esp_err_t run_err = write_regs(0x00, &run, 1);
    return err != ESP_OK ? err : run_err;
}

/* Public interface */

esp_err_t rtc_clock_init(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, DS3231_ADDRESS, PROBE_TIMEOUT_MS) == ESP_OK) {
        ESP_RETURN_ON_ERROR(add_device(bus, DS3231_ADDRESS), TAG, "add DS3231");
        s_kind = RTC_CLOCK_DS3231;
        ESP_RETURN_ON_ERROR(ds3231_setup(), TAG, "DS3231 setup");
    } else if (i2c_master_probe(bus, PCF85063_ADDRESS, PROBE_TIMEOUT_MS) == ESP_OK) {
        ESP_RETURN_ON_ERROR(add_device(bus, PCF85063_ADDRESS), TAG, "add PCF85063");
        s_kind = RTC_CLOCK_PCF85063;
        ESP_RETURN_ON_ERROR(pcf85063_setup(), TAG, "PCF85063 setup");
    } else {
        ESP_LOGW(TAG, "No clock chip on the I2C bus");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Clock chip: %s", rtc_clock_name());
    return ESP_OK;
}

rtc_clock_kind_t rtc_clock_kind(void)
{
    return s_kind;
}

const char *rtc_clock_name(void)
{
    switch (s_kind) {
    case RTC_CLOCK_DS3231:
        return "DS3231";
    case RTC_CLOCK_PCF85063:
        return "PCF85063";
    default:
        return "none";
    }
}

esp_err_t rtc_clock_read(time_t *utc)
{
    switch (s_kind) {
    case RTC_CLOCK_DS3231:
        return ds3231_read(utc);
    case RTC_CLOCK_PCF85063:
        return pcf85063_read(utc);
    default:
        return ESP_ERR_NOT_FOUND;
    }
}

esp_err_t rtc_clock_read_edge(time_t *utc)
{
    time_t first;
    esp_err_t err = rtc_clock_read(&first);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t until = esp_timer_get_time() + EDGE_TIMEOUT_US;
    while (esp_timer_get_time() < until) {
        vTaskDelay(1);
        time_t t;
        err = rtc_clock_read(&t);
        if (err != ESP_OK) {
            return err;
        }
        if (t != first) {
            *utc = t;
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t rtc_clock_write_now(void)
{
    switch (s_kind) {
    case RTC_CLOCK_DS3231:
        return ds3231_write_now();
    case RTC_CLOCK_PCF85063:
        return pcf85063_write_now();
    default:
        return ESP_ERR_NOT_FOUND;
    }
}
