// The screen image from trmnl-server for the "З сервера" screen. GET <url> gives a 400 x 300 1-bit BMP:
// four quadrants, the top left one empty (the device draws its date and time there).
// A task reads the image while the screen is on and Wi-Fi is connected: at once, then every 5 minutes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SRVIMG_ROW_BYTES 50
#define SRVIMG_HEIGHT    300
#define SRVIMG_BYTES     (SRVIMG_ROW_BYTES * SRVIMG_HEIGHT)

typedef void (*srvimg_change_cb_t)(void);

// Start the task. url: for example "http://server:15000/quad.bmp"; an empty URL gives no image.
// on_change runs in the task after each read (a new image or an error).
void srvimg_start(const char *url, srvimg_change_cb_t on_change);

// The screen is on (true) or off. The first time, the image comes at once.
void srvimg_set_active(bool active);

bool srvimg_active(void);

// Read the image at the next chance, also before its time (for example, at a wakeup in eco mode).
void srvimg_refresh(void);

// The number of finished reads, with or without success.
unsigned srvimg_read_count(void);

// True if the last read gave an image (a new one or the same as before).
bool srvimg_ok(void);

// Copy the last image to dst: SRVIMG_BYTES, the rows from top to bottom, 50 bytes for each row,
// the most significant bit first, 1 = black (the format of the gfx canvas). at gets the time of the image.
// Return false if there is no image yet.
bool srvimg_copy(uint8_t *dst, time_t *at);

// A short text about the last read for the settings page, with the last error and its time after a success;
// "" before the first read.
void srvimg_status(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
