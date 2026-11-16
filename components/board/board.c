// Board support: I2C bus, buttons, battery voltage and USB power.
#include "board.h"

#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define BOUNCE_US        30000LL
#define LONG_PRESS_US    3000000LL
#define RESET_PRESS_US   10000000LL

static i2c_master_bus_handle_t s_i2c;
static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_channel;
static adc_cali_handle_t s_adc_cali;
static TaskHandle_t s_notify_task;
static uint32_t s_buttons;
static int64_t s_pressed_at[2];  // 0 = KEY, 1 = BOOT; 0 when released

// Each button has a level interrupt: a low level while the button is up (wait for a press), a high level
// while it is down (wait for the release). The same level wakes the chip from light sleep, also from
// the automatic light sleep, so a press during sleep is not lost (an edge can be lost).
// On release, the time of the press gives the event.
static void button_isr(void *arg)
{
    const int index = (int)(uintptr_t)arg;
    const gpio_num_t pin = index ? BOARD_BOOT : BOARD_KEY;
    const int64_t now = esp_timer_get_time();
    const bool down = gpio_get_level(pin) == 0;
    // Wait for the other level. This also stops the interrupt of the present level.
    gpio_wakeup_enable(pin, down ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    if (down) {
        if (s_pressed_at[index] == 0) {
            s_pressed_at[index] = now;
        }
        return;
    }
    if (s_pressed_at[index] == 0) {
        return;
    }
    const int64_t held = now - s_pressed_at[index];
    s_pressed_at[index] = 0;
    if (held < BOUNCE_US) {
        return;
    }
    uint32_t event;
    if (index == 0) {
        event = BOARD_BUTTON_KEY;
    } else if (held >= RESET_PRESS_US) {
        event = BOARD_BUTTON_BOOT_10S;
    } else if (held >= LONG_PRESS_US) {
        event = BOARD_BUTTON_BOOT_3S;
    } else {
        event = BOARD_BUTTON_BOOT;
    }
    __atomic_fetch_or(&s_buttons, event, __ATOMIC_RELAXED);
    BaseType_t woken = pdFALSE;
    if (s_notify_task) {
        vTaskNotifyGiveFromISR(s_notify_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // The board has external pull-up resistors. The internal ones help a long cable.
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

static esp_err_t init_buttons(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_KEY) | (1ULL << BOARD_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "button GPIO");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "GPIO ISR service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_KEY, button_isr, (void *)(uintptr_t)0), TAG, "KEY handler");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOARD_BOOT, button_isr, (void *)(uintptr_t)1), TAG, "BOOT handler");
    // Wait for a press (a low level). gpio_wakeup_enable() also keeps the pull-up of the pins in sleep.
    ESP_RETURN_ON_ERROR(gpio_wakeup_enable(BOARD_KEY, GPIO_INTR_LOW_LEVEL), TAG, "KEY wakeup");
    ESP_RETURN_ON_ERROR(gpio_wakeup_enable(BOARD_BOOT, GPIO_INTR_LOW_LEVEL), TAG, "BOOT wakeup");
    ESP_RETURN_ON_ERROR(gpio_intr_enable(BOARD_KEY), TAG, "KEY interrupt");
    ESP_RETURN_ON_ERROR(gpio_intr_enable(BOARD_BOOT), TAG, "BOOT interrupt");
    return esp_sleep_enable_gpio_wakeup();
}

static esp_err_t init_vbus_sense(void)
{
#if CONFIG_RLCD_VBUS_SENSE_GPIO >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_RLCD_VBUS_SENSE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
#else
    return ESP_OK;
#endif
}

static esp_err_t init_adc(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BAT_ADC, &unit, &s_adc_channel), TAG, "ADC channel");

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_adc_channel, &chan_cfg), TAG, "ADC config");

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = s_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        // Without calibration the voltage is less accurate, but the board still works.
        ESP_LOGW(TAG, "No ADC calibration: the battery voltage is approximate");
        s_adc_cali = NULL;
    }
    return ESP_OK;
}

esp_err_t board_init(TaskHandle_t notify_task)
{
    s_notify_task = notify_task;
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(init_adc(), TAG, "battery ADC");
    ESP_RETURN_ON_ERROR(init_buttons(), TAG, "buttons");
    ESP_RETURN_ON_ERROR(init_vbus_sense(), TAG, "VBUS sense");
    return ESP_OK;
}

int board_vbus_sense(void)
{
#if CONFIG_RLCD_VBUS_SENSE_GPIO >= 0
    return gpio_get_level(CONFIG_RLCD_VBUS_SENSE_GPIO);
#else
    return -1;
#endif
}

bool board_usb_data_connected(void)
{
    return usb_serial_jtag_is_connected();
}

bool board_button_pressed(void)
{
    return gpio_get_level(BOARD_KEY) == 0 || gpio_get_level(BOARD_BOOT) == 0;
}

bool board_light_sleep(int64_t sleep_us)
{
    if (sleep_us < 1000) {
        return false;
    }
    // An event that waits: the caller takes it now. The sleep would wake only at the timer.
    if (__atomic_load_n(&s_buttons, __ATOMIC_RELAXED) != 0) {
        return false;
    }
    // A pressed button would wake the chip at once. Wait for its release event instead.
    if (gpio_get_level(BOARD_KEY) == 0 || gpio_get_level(BOARD_BOOT) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return false;
    }

    // The buttons wake the chip: board_init() turned on the GPIO wakeup. The timer gives the next update.
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_us);
    esp_light_sleep_start();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    const bool by_button = (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) != 0;

    // A button that is still down gives its interrupt now, and its release gives the event.
    // A very short press can be over before the interrupt: take it as KEY.
    vTaskDelay(pdMS_TO_TICKS(1));
    if (by_button && gpio_get_level(BOARD_KEY) != 0 && gpio_get_level(BOARD_BOOT) != 0 &&
        s_pressed_at[0] == 0 && s_pressed_at[1] == 0 && __atomic_load_n(&s_buttons, __ATOMIC_RELAXED) == 0) {
        __atomic_fetch_or(&s_buttons, BOARD_BUTTON_KEY, __ATOMIC_RELAXED);
    }
    return by_button;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

uint32_t board_take_buttons(void)
{
    return __atomic_exchange_n(&s_buttons, 0, __ATOMIC_RELAXED);
}

uint32_t board_peek_buttons(void)
{
    return __atomic_load_n(&s_buttons, __ATOMIC_RELAXED);
}

int board_battery_mv(void)
{
    const int samples = 16;
    int sum = 0;
    for (int i = 0; i < samples; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_adc_channel, &raw) != ESP_OK) {
            return -1;
        }
        int mv;
        if (s_adc_cali) {
            adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);
        } else {
            mv = raw * 3100 / 4095;
        }
        sum += mv;
    }
    // The 200K/100K divider gives one third of the battery voltage.
    return sum / samples * 3;
}
