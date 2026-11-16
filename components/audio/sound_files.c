// Uploaded WAV files in the "sounds" partition. The partition has AUDIO_SLOT_COUNT equal slots.
// A slot: a header in the first 4 KB sector, then mono 16-bit samples (little-endian) from 64 KB.
// The samples start at a 64 KB block, so that the flash erases them in blocks (faster than sectors).
// An upload erases the header first and writes it last: a broken upload leaves no file.
#include "audio.h"
#include "texts.h"
#include "sound_files.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sounds";

#define PARTITION_LABEL "sounds"
#define HEADER_SECTOR   4096
#define DATA_OFFSET     (64 * 1024)
#define ERASE_STEP      (64 * 1024)
#define SLOT_MAGIC      0x444E5352u     // "RSND"
#define OUT_SAMPLES     512

typedef struct {
    uint32_t magic;
    uint32_t rate;
    uint32_t samples;
    char name[48];
    uint32_t check;     // magic ^ rate ^ samples
} slot_header_t;

static const esp_partition_t *s_part;
static size_t s_slot_bytes;
static SemaphoreHandle_t s_lock;
static const char *s_begin_error = "";

static const unsigned RATES[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };

void sound_files_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PARTITION_LABEL);
    if (s_part) {
        s_slot_bytes = s_part->size / AUDIO_SLOT_COUNT / ERASE_STEP * ERASE_STEP;
        ESP_LOGI(TAG, "Partition \"%s\": %d slots of %u KB", PARTITION_LABEL, AUDIO_SLOT_COUNT,
                 (unsigned)(s_slot_bytes / 1024));
    } else {
        ESP_LOGW(TAG, "No \"%s\" partition: flash the new partition table over USB to upload sounds",
                 PARTITION_LABEL);
    }
}

bool audio_files_available(void)
{
    return s_lock && s_part && s_slot_bytes > DATA_OFFSET;
}

static bool valid_slot(int slot)
{
    return slot >= 0 && slot < AUDIO_SLOT_COUNT;
}

static size_t slot_base(int slot)
{
    return (size_t)slot * s_slot_bytes;
}

static size_t max_samples(void)
{
    return (s_slot_bytes - DATA_OFFSET) / sizeof(int16_t);
}

unsigned audio_file_max_seconds(unsigned rate)
{
    return audio_files_available() && rate ? (unsigned)(max_samples() / rate) : 0;
}

static bool read_header(int slot, slot_header_t *h)
{
    if (!audio_files_available() || !valid_slot(slot) ||
        esp_partition_read(s_part, slot_base(slot), h, sizeof(*h)) != ESP_OK) {
        return false;
    }
    return h->magic == SLOT_MAGIC && h->check == (SLOT_MAGIC ^ h->rate ^ h->samples) && h->rate >= 8000 &&
           h->rate <= 48000 && h->samples > 0 && h->samples <= max_samples();
}

bool sound_files_header(int slot, unsigned *rate, size_t *samples)
{
    slot_header_t h;
    if (!read_header(slot, &h)) {
        return false;
    }
    *rate = h.rate;
    *samples = h.samples;
    return true;
}

esp_err_t sound_files_read(int slot, size_t first, int16_t *buf, size_t count)
{
    if (!audio_files_available() || !valid_slot(slot) || first + count > max_samples()) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_read(s_part, slot_base(slot) + DATA_OFFSET + first * sizeof(int16_t), buf,
                              count * sizeof(int16_t));
}

bool sound_files_lock(void)
{
    return s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE;
}

void sound_files_unlock(void)
{
    xSemaphoreGive(s_lock);
}

void audio_file_info(int slot, audio_file_info_t *out)
{
    memset(out, 0, sizeof(*out));
    slot_header_t h;
    if (read_header(slot, &h)) {
        out->valid = true;
        out->rate = h.rate;
        out->tenths = (unsigned)((uint64_t)h.samples * 10 / h.rate);
        memcpy(out->name, h.name, sizeof(out->name));
        out->name[sizeof(out->name) - 1] = '\0';
    }
}

esp_err_t audio_file_delete(int slot)
{
    if (!audio_files_available() || !valid_slot(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_partition_erase_range(s_part, slot_base(slot), HEADER_SECTOR);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Slot %d: the file is deleted", slot);
    return err;
}

/* Upload */

typedef enum { W_RIFF, W_CHUNK, W_FMT, W_SKIP, W_DATA, W_DONE, W_FAILED } wav_state_t;

struct audio_upload {
    int slot;
    char name[48];
    wav_state_t st;
    uint8_t hdr[40];
    size_t hdr_len;
    size_t need;
    uint32_t left;          // bytes left in the current chunk; UINT32_MAX = to the end of the file
    bool have_fmt;
    unsigned channels;
    unsigned bits;
    unsigned rate;
    unsigned frame;         // bytes in one frame (all channels)
    uint8_t part[4];        // a frame that the last write cut
    size_t part_len;
    int16_t out[OUT_SAMPLES];
    size_t out_n;
    size_t samples;         // samples in the flash
    size_t erased;          // bytes erased from DATA_OFFSET
    char error[160];
};

static esp_err_t fail(audio_upload_t *u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t fail(audio_upload_t *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->error, sizeof(u->error), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "Upload to slot %d: %s", u->slot, u->error);
    u->st = W_FAILED;
    return ESP_FAIL;
}

// Copy a name. Do not cut a UTF-8 character at the end.
static void copy_name(char *dst, size_t size, const char *src)
{
    size_t n = strlcpy(dst, src ? src : "", size);
    if (n < size) {
        return;
    }
    n = size - 1;
    size_t start = n;
    while (start > 0 && ((unsigned char)dst[start - 1] & 0xC0) == 0x80) {
        start--;
    }
    if (start > 0 && ((unsigned char)dst[start - 1] & 0x80)) {
        const unsigned char lead = (unsigned char)dst[start - 1];
        const size_t len = lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : 2);
        if (n - (start - 1) < len) {
            n = start - 1;
        }
    }
    dst[n] = '\0';
}

audio_upload_t *audio_upload_begin(int slot, const char *name)
{
    if (!audio_files_available()) {
        s_begin_error = tr(T_SND_NO_PARTITION);
        return NULL;
    }
    if (!valid_slot(slot)) {
        s_begin_error = tr(T_SND_BAD_SLOT);
        return NULL;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        s_begin_error = tr(T_SND_BUSY);
        return NULL;
    }
    audio_upload_t *u = calloc(1, sizeof(*u));
    if (!u) {
        xSemaphoreGive(s_lock);
        s_begin_error = tr(T_MSG_NO_MEMORY);
        return NULL;
    }
    u->slot = slot;
    copy_name(u->name, sizeof(u->name), name);
    // The old file is gone from now on.
    if (esp_partition_erase_range(s_part, slot_base(slot), HEADER_SECTOR) != ESP_OK) {
        free(u);
        xSemaphoreGive(s_lock);
        s_begin_error = tr(T_SND_ERASE_OLD);
        return NULL;
    }
    u->need = 12;
    u->st = W_RIFF;
    ESP_LOGI(TAG, "Upload \"%s\" to slot %d", u->name, slot);
    return u;
}

const char *audio_upload_begin_error(void)
{
    return s_begin_error;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Collect u->need header bytes. Return true when they are all in u->hdr.
static bool collect(audio_upload_t *u, const uint8_t **p, size_t *len)
{
    const size_t take = u->need - u->hdr_len < *len ? u->need - u->hdr_len : *len;
    memcpy(u->hdr + u->hdr_len, *p, take);
    u->hdr_len += take;
    *p += take;
    *len -= take;
    return u->hdr_len == u->need;
}

static void next_chunk(audio_upload_t *u)
{
    u->hdr_len = 0;
    u->need = 8;
    u->st = W_CHUNK;
}

static bool rate_supported(unsigned rate)
{
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]); i++) {
        if (RATES[i] == rate) {
            return true;
        }
    }
    return false;
}

static esp_err_t parse_fmt(audio_upload_t *u)
{
    const uint8_t *h = u->hdr;
    unsigned format = le16(h);
    const unsigned channels = le16(h + 2), rate = le32(h + 4), bits = le16(h + 14);
    if (format == 0xFFFE && u->need >= 26) {
        format = le16(h + 24);  // WAVE_FORMAT_EXTENSIBLE: the sub-format
    }
    if (format != 1) {
        return fail(u, tr(T_SND_NEED_PCM));
    }
    if (channels < 1 || channels > 2) {
        return fail(u, tr(T_SND_CHANNELS), channels);
    }
    if (bits != 8 && bits != 16) {
        return fail(u, tr(T_SND_BITS), bits);
    }
    if (!rate_supported(rate)) {
        return fail(u, tr(T_SND_RATE), rate);
    }
    u->channels = channels;
    u->bits = bits;
    u->rate = rate;
    u->frame = channels * bits / 8;
    u->have_fmt = true;
    return ESP_OK;
}

static esp_err_t flush(audio_upload_t *u)
{
    if (u->out_n == 0) {
        return ESP_OK;
    }
    if (u->samples + u->out_n > max_samples()) {
        return fail(u, tr(T_SND_TOO_LONG), u->rate, audio_file_max_seconds(u->rate));
    }
    const size_t offset = u->samples * sizeof(int16_t);
    const size_t bytes = u->out_n * sizeof(int16_t);
    const size_t base = slot_base(u->slot) + DATA_OFFSET;
    while (offset + bytes > u->erased) {
        const size_t room = s_slot_bytes - DATA_OFFSET - u->erased;
        const size_t step = room < ERASE_STEP ? room : ERASE_STEP;
        const esp_err_t err = esp_partition_erase_range(s_part, base + u->erased, step);
        if (err != ESP_OK) {
            return fail(u, tr(T_SND_ERASE_FAILED), esp_err_to_name(err));
        }
        u->erased += step;
    }
    const esp_err_t err = esp_partition_write(s_part, base + offset, u->out, bytes);
    if (err != ESP_OK) {
        return fail(u, tr(T_SND_WRITE_FAILED), esp_err_to_name(err));
    }
    u->samples += u->out_n;
    u->out_n = 0;
    return ESP_OK;
}

// Mix the channels of one frame into one 16-bit sample.
static int16_t frame_sample(const audio_upload_t *u, const uint8_t *f)
{
    int32_t sum = 0;
    for (unsigned c = 0; c < u->channels; c++) {
        if (u->bits == 16) {
            sum += (int16_t)le16(f + 2 * c);
        } else {
            sum += ((int32_t)f[c] - 128) * 256;
        }
    }
    return (int16_t)(sum / (int32_t)u->channels);
}

static esp_err_t add_sample(audio_upload_t *u, int16_t sample)
{
    u->out[u->out_n++] = sample;
    return u->out_n == OUT_SAMPLES ? flush(u) : ESP_OK;
}

static esp_err_t pcm(audio_upload_t *u, const uint8_t *p, size_t n)
{
    while (n > 0) {
        if (u->part_len > 0 || n < u->frame) {
            const size_t take = u->frame - u->part_len < n ? u->frame - u->part_len : n;
            memcpy(u->part + u->part_len, p, take);
            u->part_len += take;
            p += take;
            n -= take;
            if (u->part_len < u->frame) {
                return ESP_OK;
            }
            u->part_len = 0;
            if (add_sample(u, frame_sample(u, u->part)) != ESP_OK) {
                return ESP_FAIL;
            }
            continue;
        }
        if (add_sample(u, frame_sample(u, p)) != ESP_OK) {
            return ESP_FAIL;
        }
        p += u->frame;
        n -= u->frame;
    }
    return ESP_OK;
}

esp_err_t audio_upload_write(audio_upload_t *u, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        switch (u->st) {
        case W_RIFF:
            if (!collect(u, &p, &len)) {
                return ESP_OK;
            }
            if (memcmp(u->hdr, "RIFF", 4) != 0 || memcmp(u->hdr + 8, "WAVE", 4) != 0) {
                return fail(u, tr(T_SND_NOT_WAV));
            }
            next_chunk(u);
            break;
        case W_CHUNK: {
            if (!collect(u, &p, &len)) {
                return ESP_OK;
            }
            const uint32_t size = le32(u->hdr + 4);
            if (memcmp(u->hdr, "fmt ", 4) == 0) {
                if (size < 16 || size > sizeof(u->hdr)) {
                    return fail(u, tr(T_SND_ODD_HEADER), (unsigned)size);
                }
                u->hdr_len = 0;
                u->need = size;
                u->left = size & 1;     // a pad byte after an odd chunk
                u->st = W_FMT;
            } else if (memcmp(u->hdr, "data", 4) == 0) {
                if (!u->have_fmt) {
                    return fail(u, tr(T_SND_NO_FORMAT));
                }
                // Some programs write 0 or 0xFFFFFFFF for a stream: then take the rest of the file.
                u->left = size == 0 || size == UINT32_MAX ? UINT32_MAX : size;
                u->st = W_DATA;
            } else {
                u->left = size + (size & 1);
                u->st = W_SKIP;
            }
            break;
        }
        case W_FMT:
            if (!collect(u, &p, &len)) {
                return ESP_OK;
            }
            if (parse_fmt(u) != ESP_OK) {
                return ESP_FAIL;
            }
            if (u->left) {
                u->st = W_SKIP;
            } else {
                next_chunk(u);
            }
            break;
        case W_SKIP: {
            const size_t take = u->left < len ? u->left : len;
            p += take;
            len -= take;
            u->left -= (uint32_t)take;
            if (u->left == 0) {
                next_chunk(u);
            }
            break;
        }
        case W_DATA: {
            const size_t take = u->left < len ? u->left : len;
            if (pcm(u, p, take) != ESP_OK) {
                return ESP_FAIL;
            }
            p += take;
            len -= take;
            if (u->left != UINT32_MAX) {
                u->left -= (uint32_t)take;
                if (u->left == 0) {
                    u->st = W_DONE;
                }
            }
            break;
        }
        case W_DONE:
            return ESP_OK;      // chunks after the sound (for example, LIST) are not necessary
        case W_FAILED:
        default:
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t audio_upload_finish(audio_upload_t *u)
{
    if (u->st == W_FAILED) {
        return ESP_FAIL;
    }
    if (u->st != W_DATA && u->st != W_DONE) {
        return fail(u, tr(T_SND_ENDED_EARLY));
    }
    if (u->st == W_DATA && u->left != UINT32_MAX && u->left > 0) {
        return fail(u, tr(T_SND_CUT));
    }
    if (flush(u) != ESP_OK) {
        return ESP_FAIL;
    }
    if (u->samples == 0) {
        return fail(u, tr(T_SND_EMPTY));
    }
    slot_header_t h = {
        .magic = SLOT_MAGIC,
        .rate = u->rate,
        .samples = (uint32_t)u->samples,
        .check = SLOT_MAGIC ^ u->rate ^ (uint32_t)u->samples,
    };
    memcpy(h.name, u->name, sizeof(h.name));
    const esp_err_t err = esp_partition_write(s_part, slot_base(u->slot), &h, sizeof(h));
    if (err != ESP_OK) {
        return fail(u, tr(T_SND_HEADER_FAILED), esp_err_to_name(err));
    }
    u->st = W_DONE;
    ESP_LOGI(TAG, "Slot %d: %u samples at %u Hz", u->slot, (unsigned)u->samples, u->rate);
    return ESP_OK;
}

const char *audio_upload_error(const audio_upload_t *u)
{
    return u->error;
}

void audio_upload_end(audio_upload_t *u)
{
    if (u) {
        free(u);
        xSemaphoreGive(s_lock);
    }
}
