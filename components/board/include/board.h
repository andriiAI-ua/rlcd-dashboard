// Board support for the Waveshare ESP32-S3-RLCD-4.2.
// The pin numbers come from the board schematic (ESP32-S3-RLCD-4.2-schematic.pdf).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reflective LCD, ST7305 controller, SPI
#define BOARD_LCD_SCK   GPIO_NUM_11
#define BOARD_LCD_MOSI  GPIO_NUM_12
#define BOARD_LCD_DC    GPIO_NUM_5
#define BOARD_LCD_CS    GPIO_NUM_40
#define BOARD_LCD_RST   GPIO_NUM_41
#define BOARD_LCD_TE    GPIO_NUM_6

// I2C bus: SHTC3 (0x70), PCF85063 (0x51), ES8311 (0x18), ES7210 (0x40).
// The 2x8 header also has SDA and SCL, for example for a DS3231 module (0x68).
#define BOARD_I2C_SDA   GPIO_NUM_13
#define BOARD_I2C_SCL   GPIO_NUM_14
#define BOARD_RTC_INT   GPIO_NUM_15

// Buttons. Both are active low.
#define BOARD_KEY       GPIO_NUM_18
#define BOARD_BOOT      GPIO_NUM_0

// Battery voltage through a 200K/100K divider (ADC voltage x 3)
#define BOARD_BAT_ADC   GPIO_NUM_4

// Audio: ES8311 codec, NS4150B amplifier, 8 ohm speaker on the SPK connector
#define BOARD_I2S_MCLK  GPIO_NUM_16
#define BOARD_I2S_SCLK  GPIO_NUM_9
#define BOARD_I2S_LRCK  GPIO_NUM_45
#define BOARD_I2S_DOUT  GPIO_NUM_8
#define BOARD_I2S_DIN   GPIO_NUM_10
#define BOARD_PA_CTRL   GPIO_NUM_46

// Button events for board_take_buttons(). An event comes when the button is released.
#define BOARD_BUTTON_KEY      (1u << 0)  // KEY, any press
#define BOARD_BUTTON_BOOT     (1u << 1)  // BOOT, shorter than 3 s
#define BOARD_BUTTON_BOOT_3S  (1u << 2)  // BOOT, 3 to 10 s
#define BOARD_BUTTON_BOOT_10S (1u << 3)  // BOOT, 10 s or more

// Start the I2C bus, the battery ADC and the buttons.
// A button press sends a task notification to notify_task.
esp_err_t board_init(TaskHandle_t notify_task);

// Return the shared I2C bus.
i2c_master_bus_handle_t board_i2c_bus(void);

// Return the buttons pressed since the last call (BOARD_BUTTON_* bits) and clear them.
uint32_t board_take_buttons(void);

// Return the button events that wait for board_take_buttons(), and keep them.
uint32_t board_peek_buttons(void);

// Return the battery voltage in millivolts, or -1 if the ADC read fails.
int board_battery_mv(void);

// The board has no signal for USB power (VBUS goes only to the charger and the header).
// Return 1 or 0 from the optional VBUS divider (CONFIG_RLCD_VBUS_SENSE_GPIO), or -1 if there is none.
int board_vbus_sense(void);

// Return true when a computer is connected to the USB port (USB data frames come in).
// A charger or a power bank gives no data frames, so this is false for them.
bool board_usb_data_connected(void);

// Return true while KEY or BOOT is pressed.
bool board_button_pressed(void);

// Put the chip into light sleep for sleep_us or until KEY or BOOT is pressed. Turn Wi-Fi off before.
// The screen keeps its image. Return true if a button woke the chip; its event comes as usual
// from board_take_buttons(). Return false without sleep if a button is pressed now or its event waits.
bool board_light_sleep(int64_t sleep_us);

#ifdef __cplusplus
}
#endif
