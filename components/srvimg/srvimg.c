// The screen image from trmnl-server (see srvimg.h).
#include "srvimg.h"
#include "texts.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"

static const char *TAG = "srvimg";

#define WIDTH       400
#define PERIOD_S    300         // a new image every 5 minutes
#define RETRY_S     60          // after an error, try again in 1 minute
#define WAIT_NET_MS 5000        // without Wi-Fi, look again after this time
#define MAX_FILE    20000       // a 400 x 300 1-bit BMP has 15662 bytes
#define VALID_TIME  1704067200  // the time is not known before 2024-01-01

static char s_url[256];
static srvimg_change_cb_t s_on_change;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;

// Under s_lock.
static uint8_t *s_image;        // SRVIMG_BYTES in PSRAM
static bool s_have_image;
static time_t s_image_at;
static char s_status[96];
static char s_last_error[112];  // the last problem with its time, "" = none since the start

static volatile bool s_active;
static volatile bool s_refresh;
static volatile unsigned s_reads;
static volatile bool s_ok;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

// Check a 400 x 300 1-bit BMP and change it to the canvas format. Return NULL, or a text about the problem.
static const char *bmp_to_canvas(const uint8_t *bmp, int size, uint8_t *out)
{
    if (size < 62 || bmp[0] != 'B' || bmp[1] != 'M') {
        return tr(T_IMG_NOT_BMP);
    }
    const uint32_t offset = le32(bmp + 10);
    const uint32_t header = le32(bmp + 14);
    const int32_t width = (int32_t)le32(bmp + 18);
    const int32_t height = (int32_t)le32(bmp + 22);
    if (width != WIDTH || (height != SRVIMG_HEIGHT && height != -SRVIMG_HEIGHT)) {
        return tr(T_IMG_BAD_SIZE);
    }
    if (le16(bmp + 28) != 1 || le32(bmp + 30) != 0) {
        return tr(T_IMG_NOT_1BIT);
    }
    // The palette follows the header: color 0 and color 1, 4 bytes each (blue, green, red, 0).
    const uint32_t palette = 14 + header;
    // Each row has a length of a multiple of 4 bytes: 52 bytes for 400 pixels.
    const uint32_t stride = (WIDTH + 31) / 32 * 4;
    if (palette + 8 > offset || offset + stride * SRVIMG_HEIGHT > (uint32_t)size) {
        return tr(T_IMG_SHORT);
    }
    const uint8_t *pal = bmp + palette;
    const bool one_is_black = pal[4] + pal[5] + pal[6] < pal[0] + pal[1] + pal[2];
    for (int y = 0; y < SRVIMG_HEIGHT; y++) {
        // A positive height: the rows go from the bottom to the top.
        const int row = height > 0 ? SRVIMG_HEIGHT - 1 - y : y;
        const uint8_t *src = bmp + offset + (size_t)row * stride;
        uint8_t *dst = out + y * SRVIMG_ROW_BYTES;
        for (int i = 0; i < SRVIMG_ROW_BYTES; i++) {
            dst[i] = one_is_black ? src[i] : (uint8_t)~src[i];
        }
    }
    return NULL;
}

// Read the body of GET s_url to buf. Return the byte count, or -1 (err gets the problem).
static int http_get(uint8_t *buf, int len, char *err, size_t err_len)
{
    const esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        strlcpy(err, tr(T_IMG_NO_MEMORY), err_len);
        return -1;
    }
    int total = -1;
    const esp_err_t open = esp_http_client_open(client, 0);
    if (open != ESP_OK) {
        snprintf(err, err_len, tr(T_IMG_NO_LINK), esp_err_to_name(open));
    } else {
        const int64_t length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (length < 0) {
            // -1 is ESP_FAIL; other errors come as a negative ESP-IDF code.
            snprintf(err, err_len, tr(T_IMG_NO_ANSWER), esp_err_to_name(length == -1 ? ESP_FAIL : (esp_err_t)-length));
        } else if (status != 200) {
            snprintf(err, err_len, "HTTP %d", status);
        } else {
            total = 0;
            for (;;) {
                const int n = esp_http_client_read(client, (char *)buf + total, len - total);
                if (n < 0) {
                    strlcpy(err, tr(T_IMG_LINK_LOST), err_len);
                    total = -1;
                    break;
                }
                if (n == 0) {
                    break;
                }
                total += n;
                if (total >= len) {
                    strlcpy(err, tr(T_IMG_TOO_BIG), err_len);
                    total = -1;
                    break;
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total;
}

static void hhmm(char *buf, size_t len, time_t t)
{
    if (t < VALID_TIME) {
        snprintf(buf, len, "--:--");
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// Read the image once and keep it. Return true on success.
static bool read_image(uint8_t *file, uint8_t *image)
{
    char err[64] = "";
    const int n = http_get(file, MAX_FILE, err, sizeof(err));
    const char *problem = n < 0 ? err : bmp_to_canvas(file, n, image);
    const time_t now = time(NULL);
    char hm[8];
    hhmm(hm, sizeof(hm), now);

    bool same = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!problem) {
        // The same image is also news: the data of the server is still current.
        same = s_have_image && memcmp(s_image, image, SRVIMG_BYTES) == 0;
        if (!same) {
            memcpy(s_image, image, SRVIMG_BYTES);
        }
        s_have_image = true;
        s_image_at = now;
        snprintf(s_status, sizeof(s_status), tr(T_IMG_AT), same ? tr(T_IMG_SAME) : tr(T_IMG_NEW), hm);
    } else {
        snprintf(s_status, sizeof(s_status), tr(T_IMG_AT), problem, hm);
        snprintf(s_last_error, sizeof(s_last_error), tr(T_IMG_AT), problem, hm);
    }
    xSemaphoreGive(s_lock);
    s_ok = problem == NULL;

    if (problem) {
        ESP_LOGW(TAG, "%s: %s", s_url, problem);
    } else {
        ESP_LOGI(TAG, "%s image (%d bytes)", same ? "The same" : "A new", n);
    }
    return problem == NULL;
}

static void srvimg_task(void *arg)
{
    (void)arg;
    uint8_t *file = heap_caps_malloc(MAX_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *image = heap_caps_malloc(SRVIMG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file || !image) {
        ESP_LOGE(TAG, "No memory for the image");
        vTaskDelete(NULL);
        return;
    }
    int64_t next_us = 0;    // the time of the next read (esp_timer_get_time())
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_active && s_url[0]) {
            if (!net_connected()) {
                wait = pdMS_TO_TICKS(WAIT_NET_MS);
            } else if (s_refresh || esp_timer_get_time() >= next_us) {
                s_refresh = false;
                const bool ok = read_image(file, image);
                next_us = esp_timer_get_time() + (int64_t)(ok ? PERIOD_S : RETRY_S) * 1000000;
                s_reads++;
                if (s_on_change) {
                    s_on_change();
                }
                continue;
            } else {
                const int64_t left_ms = (next_us - esp_timer_get_time()) / 1000;
                wait = pdMS_TO_TICKS(left_ms > 1000 ? left_ms : 1000);
            }
        }
        // srvimg_set_active() and srvimg_refresh() wake the task before the time.
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

void srvimg_start(const char *url, srvimg_change_cb_t on_change)
{
    strlcpy(s_url, url ? url : "", sizeof(s_url));
    s_on_change = on_change;
    s_lock = xSemaphoreCreateMutex();
    s_image = heap_caps_malloc(SRVIMG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lock || !s_image) {
        ESP_LOGE(TAG, "No memory for the image");
        return;
    }
    xTaskCreate(srvimg_task, "srvimg", 6144, NULL, 3, &s_task);
}

void srvimg_set_active(bool active)
{
    if (s_active == active) {
        return;
    }
    s_active = active;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool srvimg_active(void)
{
    return s_active;
}

void srvimg_refresh(void)
{
    s_refresh = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

unsigned srvimg_read_count(void)
{
    return s_reads;
}

bool srvimg_ok(void)
{
    return s_ok;
}

bool srvimg_copy(uint8_t *dst, time_t *at)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = s_have_image;
    if (have) {
        memcpy(dst, s_image, SRVIMG_BYTES);
        *at = s_image_at;
    }
    xSemaphoreGive(s_lock);
    return have;
}

void srvimg_status(char *buf, size_t len)
{
    buf[0] = '\0';
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ok && s_last_error[0]) {
        snprintf(buf, len, tr(T_IMG_LAST_ERROR), s_status, s_last_error);
    } else {
        strlcpy(buf, s_status, len);
    }
    xSemaphoreGive(s_lock);
}
