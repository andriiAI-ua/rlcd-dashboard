// Firmware update from a release archive (see fwupdate.h).
#include "fwupdate.h"
#include "texts.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "fwupdate";

#define TAR_BLOCK       512
#define MANIFEST_NAME   "manifest.json"
#define MANIFEST_MAX    1024
#define IMAGE_HEAD      1024    // the start of the image that the checks need
#define SHA256_LEN      32

#define DL_CHUNK        4096
#define DL_TIMEOUT_MS   15000
#define DL_MAX_WAITS    4       // read timeouts in a row before the download stops
#define DL_REDIRECTS    5
#define DL_STACK        8192

typedef enum {
    ST_HEADER,      // read the 512-byte header of the next entry
    ST_MANIFEST,    // read manifest.json
    ST_IMAGE,       // check and write the image
    ST_SKIP,        // skip data or padding
    ST_END,         // the end-of-archive block came
    ST_FAILED,
} parse_state_t;

struct fwupdate {
    const esp_partition_t *part;
    esp_ota_handle_t ota;
    bool ota_started;       // esp_ota_begin() done, esp_ota_end() not yet
    bool finished;          // the new slot is the boot slot
    psa_hash_operation_t hash;
    bool hash_started;

    parse_state_t st;
    uint8_t block[TAR_BLOCK];
    size_t block_len;
    size_t left;            // bytes left in the data of the current entry
    size_t pad;             // padding bytes after the data of the current entry

    char manifest[MANIFEST_MAX];
    size_t manifest_len;
    bool have_manifest;
    char version[32];
    char file[64];
    size_t size;
    uint8_t sha256[SHA256_LEN];

    uint8_t head[IMAGE_HEAD];
    size_t head_len;
    bool head_done;         // the checks of the image start passed, the writing started
    bool image_seen;
    bool image_ok;          // the image is complete and its SHA-256 is correct

    char error[160];
};

static atomic_bool s_active;
static fwupdate_notify_fn_t s_notify;
static const char *s_begin_error = "";

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static fwupdate_status_t s_status;

/* Common */

static void notify(void)
{
    if (s_notify) {
        s_notify();
    }
}

void fwupdate_set_notify(fwupdate_notify_fn_t fn)
{
    s_notify = fn;
}

bool fwupdate_active(void)
{
    return atomic_load(&s_active);
}

// Take the right to update. Return false if an update runs already.
static bool reserve(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&s_active, &expected, true);
}

static void release(void)
{
    atomic_store(&s_active, false);
}

static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

// Record the error and stop the update. Always return ESP_FAIL.
static esp_err_t fail(fwupdate_t *u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t fail(fwupdate_t *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->error, sizeof(u->error), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", u->error);
    u->st = ST_FAILED;
    if (u->ota_started) {
        esp_ota_abort(u->ota);
        u->ota_started = false;
    }
    return ESP_FAIL;
}

/* Archive */

// Make the update state. The caller has the right to update (reserve()).
static fwupdate_t *create(void)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        s_begin_error = tr(T_FW_NO_PARTITION);
        return NULL;
    }
    fwupdate_t *u = calloc(1, sizeof(*u));
    if (!u) {
        s_begin_error = tr(T_MSG_NO_MEMORY);
        return NULL;
    }
    u->part = part;
    u->hash = psa_hash_operation_init();
    psa_crypto_init();      // the system starts PSA; a second call does nothing
    if (psa_hash_setup(&u->hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(u);
        s_begin_error = tr(T_FW_SHA_START);
        return NULL;
    }
    u->hash_started = true;
    u->st = ST_HEADER;
    return u;
}

fwupdate_t *fwupdate_begin(void)
{
    if (!reserve()) {
        s_begin_error = tr(T_MSG_OTA_RUNNING);
        return NULL;
    }
    fwupdate_t *u = create();
    if (!u) {
        release();
    }
    return u;
}

const char *fwupdate_begin_error(void)
{
    return s_begin_error;
}

// Read a tar number: octal digits, then spaces or NUL characters.
static bool parse_octal(const uint8_t *p, size_t n, size_t *out)
{
    size_t i = 0;
    while (i < n && p[i] == ' ') {
        i++;
    }
    if (i == n || p[i] < '0' || p[i] > '7') {
        return false;
    }
    size_t v = 0;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > (SIZE_MAX >> 3)) {
            return false;
        }
        v = v * 8 + (size_t)(p[i] - '0');
    }
    for (; i < n; i++) {
        if (p[i] != ' ' && p[i] != '\0') {
            return false;
        }
    }
    *out = v;
    return true;
}

static bool ends_with(const char *s, const char *end)
{
    const size_t a = strlen(s);
    const size_t b = strlen(end);
    return a >= b && strcmp(s + a - b, end) == 0;
}

// After the data of an entry, skip its padding, then read the next header.
static void after_data(fwupdate_t *u)
{
    u->left = u->pad;
    u->pad = 0;
    u->st = u->left ? ST_SKIP : ST_HEADER;
}

// Skip the data and the padding of the current entry.
static void skip_entry(fwupdate_t *u, size_t size)
{
    u->left = size + u->pad;
    u->pad = 0;
    u->st = u->left ? ST_SKIP : ST_HEADER;
}

static esp_err_t parse_header(fwupdate_t *u)
{
    const uint8_t *h = u->block;
    bool zero = true;
    for (size_t i = 0; i < TAR_BLOCK && zero; i++) {
        zero = h[i] == 0;
    }
    if (zero) {
        u->st = ST_END;
        return ESP_OK;
    }
    if (memcmp(h + 257, "ustar", 5) != 0) {
        if (h[0] == ESP_IMAGE_HEADER_MAGIC) {
            return fail(u, tr(T_FW_BIN_FILE));
        }
        return fail(u, tr(T_FW_NOT_TAR));
    }

    // Header checksum: the sum of all bytes, with the checksum field as spaces.
    size_t check;
    if (!parse_octal(h + 148, 8, &check)) {
        return fail(u, tr(T_FW_BAD_HEADER));
    }
    unsigned sum = 0;
    int signed_sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        const uint8_t b = (i >= 148 && i < 156) ? ' ' : h[i];
        sum += b;
        signed_sum += (int8_t)b;
    }
    if (check != sum && (signed_sum < 0 || check != (size_t)signed_sum)) {
        return fail(u, tr(T_FW_BAD_CHECKSUM));
    }

    size_t size;
    if ((h[124] & 0x80) || !parse_octal(h + 124, 12, &size)) {
        return fail(u, tr(T_FW_BAD_SIZE));
    }
    char name[101];
    memcpy(name, h, 100);
    name[100] = '\0';
    const char *n = strncmp(name, "./", 2) == 0 ? name + 2 : name;
    const char type = (char)h[156];
    u->pad = (TAR_BLOCK - size % TAR_BLOCK) % TAR_BLOCK;

    // A directory, a PAX or a GNU extension entry: skip it.
    if (type != '0' && type != '\0') {
        skip_entry(u, size);
        return ESP_OK;
    }
    if (strcmp(n, MANIFEST_NAME) == 0) {
        if (u->have_manifest) {
            return fail(u, tr(T_FW_TWO_MANIFESTS));
        }
        if (size == 0 || size >= MANIFEST_MAX) {
            return fail(u, tr(T_FW_MANIFEST_SIZE));
        }
        u->left = size;
        u->st = ST_MANIFEST;
        return ESP_OK;
    }
    if (u->have_manifest && strcmp(n, u->file) == 0) {
        if (u->image_seen) {
            return fail(u, tr(T_FW_TWO_FILES), u->file);
        }
        if (size != u->size) {
            return fail(u, tr(T_FW_SIZE_MISMATCH), u->file, (unsigned)size,
                        (unsigned)u->size);
        }
        u->image_seen = true;
        u->left = size;
        u->st = ST_IMAGE;
        return ESP_OK;
    }
    if (!u->have_manifest && ends_with(n, ".bin")) {
        return fail(u, tr(T_FW_MANIFEST_FIRST));
    }
    skip_entry(u, size);
    return ESP_OK;
}

static bool parse_sha256(const char *hex, uint8_t *out)
{
    if (strlen(hex) != SHA256_LEN * 2) {
        return false;
    }
    for (size_t i = 0; i < SHA256_LEN * 2; i++) {
        const char c = hex[i];
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return false;
        }
        if (i % 2 == 0) {
            out[i / 2] = (uint8_t)(v << 4);
        } else {
            out[i / 2] |= (uint8_t)v;
        }
    }
    return true;
}

static esp_err_t parse_manifest(fwupdate_t *u)
{
    cJSON *root = cJSON_ParseWithLength(u->manifest, u->manifest_len);
    if (!root) {
        return fail(u, tr(T_FW_MANIFEST_BROKEN));
    }
    const cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *chip = cJSON_GetObjectItemCaseSensitive(root, "chip");
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");

    esp_err_t err = ESP_OK;
    if (!cJSON_IsString(project) || !cJSON_IsString(version) || !cJSON_IsString(chip) || !cJSON_IsString(file) ||
        !cJSON_IsNumber(size) || !cJSON_IsString(sha)) {
        err = fail(u, tr(T_FW_MANIFEST_FIELDS));
    } else if (strcmp(project->valuestring, esp_app_get_description()->project_name) != 0) {
        err = fail(u, tr(T_MSG_OTHER_PROJECT), project->valuestring);
    } else if (strcmp(chip->valuestring, CONFIG_IDF_TARGET) != 0) {
        err = fail(u, tr(T_MSG_OTHER_CHIP), chip->valuestring);
    } else if (version->valuestring[0] == '\0' || strlen(version->valuestring) >= sizeof(u->version)) {
        err = fail(u, tr(T_FW_BAD_VERSION));
    } else if (file->valuestring[0] == '\0' || strlen(file->valuestring) >= sizeof(u->file)) {
        err = fail(u, tr(T_FW_BAD_NAME));
    } else if (size->valuedouble < 1) {
        err = fail(u, tr(T_FW_BAD_MANIFEST_SIZE));
    } else if (size->valuedouble > (double)u->part->size) {
        err = fail(u, tr(T_FW_TOO_BIG), size->valuedouble, u->part->label,
                   (unsigned)u->part->size);
    } else if (!parse_sha256(sha->valuestring, u->sha256)) {
        err = fail(u, tr(T_FW_BAD_SHA));
    } else {
        strlcpy(u->version, version->valuestring, sizeof(u->version));
        strlcpy(u->file, file->valuestring, sizeof(u->file));
        u->size = (size_t)size->valuedouble;
        u->have_manifest = true;
        ESP_LOGI(TAG, "Manifest: %s %s, %s, %u bytes", project->valuestring, u->version, u->file,
                 (unsigned)u->size);
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t write_image(fwupdate_t *u, const uint8_t *data, size_t len)
{
    if (psa_hash_update(&u->hash, data, len) != PSA_SUCCESS) {
        return fail(u, tr(T_FW_SHA_FAILED));
    }
    const esp_err_t err = esp_ota_write(u->ota, data, len);
    if (err != ESP_OK) {
        return fail(u, tr(T_FW_WRITE_FAILED), esp_err_to_name(err));
    }
    return ESP_OK;
}

// Check the start of the image (chip, project, version), then start to write the slot.
// The flash is not erased before this check.
static esp_err_t check_head(fwupdate_t *u)
{
    const size_t desc_at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (u->head_len < desc_at + sizeof(esp_app_desc_t)) {
        return fail(u, tr(T_FW_TOO_SMALL));
    }
    const esp_image_header_t *h = (const esp_image_header_t *)u->head;
    if (h->magic != ESP_IMAGE_HEADER_MAGIC) {
        return fail(u, tr(T_FW_NOT_ESP32), u->file);
    }
    if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return fail(u, tr(T_FW_OTHER_TARGET), CONFIG_IDF_TARGET);
    }
    esp_app_desc_t d;
    memcpy(&d, u->head + desc_at, sizeof(d));
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return fail(u, tr(T_FW_NOT_APP), u->file);
    }
    if (strncmp(d.project_name, esp_app_get_description()->project_name, sizeof(d.project_name)) != 0) {
        return fail(u, tr(T_FW_OTHER_PROJECT_FILE));
    }
    char version[sizeof(d.version) + 1];
    memcpy(version, d.version, sizeof(d.version));
    version[sizeof(d.version)] = '\0';
    if (strcmp(version, u->version) != 0) {
        return fail(u, tr(T_FW_VERSION_MISMATCH), version, u->version);
    }

    const esp_err_t err = esp_ota_begin(u->part, OTA_WITH_SEQUENTIAL_WRITES, &u->ota);
    if (err != ESP_OK) {
        return fail(u, tr(T_FW_BEGIN_FAILED), esp_err_to_name(err));
    }
    u->ota_started = true;
    u->head_done = true;
    ESP_LOGI(TAG, "Write version %s (%u bytes) to %s", u->version, (unsigned)u->size, u->part->label);
    notify();
    return write_image(u, u->head, u->head_len);
}

static esp_err_t image_data(fwupdate_t *u, const uint8_t *p, size_t n)
{
    if (!u->head_done) {
        const size_t take = min_size(n, IMAGE_HEAD - u->head_len);
        memcpy(u->head + u->head_len, p, take);
        u->head_len += take;
        p += take;
        n -= take;
        if (u->head_len == IMAGE_HEAD || u->head_len == u->size) {
            const esp_err_t err = check_head(u);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return n ? write_image(u, p, n) : ESP_OK;
}

static esp_err_t image_end(fwupdate_t *u)
{
    uint8_t sum[SHA256_LEN];
    size_t len = 0;
    if (psa_hash_finish(&u->hash, sum, sizeof(sum), &len) != PSA_SUCCESS || len != SHA256_LEN) {
        return fail(u, tr(T_FW_SHA_FAILED));
    }
    u->hash_started = false;
    if (memcmp(sum, u->sha256, SHA256_LEN) != 0) {
        return fail(u, tr(T_FW_SHA_MISMATCH));
    }
    u->image_ok = true;
    ESP_LOGI(TAG, "SHA-256 is correct");
    return ESP_OK;
}

esp_err_t fwupdate_write(fwupdate_t *u, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        size_t take = 0;
        esp_err_t err = ESP_OK;
        switch (u->st) {
        case ST_HEADER:
            take = min_size(len, TAR_BLOCK - u->block_len);
            memcpy(u->block + u->block_len, p, take);
            u->block_len += take;
            if (u->block_len == TAR_BLOCK) {
                u->block_len = 0;
                err = parse_header(u);
            }
            break;
        case ST_MANIFEST:
            take = min_size(len, u->left);
            memcpy(u->manifest + u->manifest_len, p, take);
            u->manifest_len += take;
            u->left -= take;
            if (u->left == 0) {
                err = parse_manifest(u);
                if (err == ESP_OK) {
                    after_data(u);
                }
            }
            break;
        case ST_IMAGE:
            take = min_size(len, u->left);
            err = image_data(u, p, take);
            u->left -= take;
            if (err == ESP_OK && u->left == 0) {
                err = image_end(u);
                if (err == ESP_OK) {
                    after_data(u);
                }
            }
            break;
        case ST_SKIP:
            take = min_size(len, u->left);
            u->left -= take;
            if (u->left == 0) {
                u->st = ST_HEADER;
            }
            break;
        case ST_END:
            return ESP_OK;      // the end blocks and the record padding
        case ST_FAILED:
        default:
            return ESP_FAIL;
        }
        if (err != ESP_OK) {
            return err;
        }
        p += take;
        len -= take;
    }
    return ESP_OK;
}

esp_err_t fwupdate_finish(fwupdate_t *u)
{
    if (u->st == ST_FAILED) {
        return ESP_FAIL;
    }
    if (!u->have_manifest) {
        return fail(u, tr(T_FW_NO_MANIFEST));
    }
    if (!u->image_seen) {
        return fail(u, tr(T_FW_NO_FILE), u->file);
    }
    if (!u->image_ok) {
        return fail(u, tr(T_FW_CUT));
    }
    // esp_ota_end() checks the image (segments and the appended hash). It frees the handle in all cases.
    esp_err_t err = esp_ota_end(u->ota);
    u->ota_started = false;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
#if CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
        return fail(u, tr(T_FW_NOT_SIGNED));
#else
        return fail(u, tr(T_FW_VERIFY_FAILED));
#endif
    }
    if (err != ESP_OK) {
        return fail(u, tr(T_FW_END_FAILED), esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(u->part);
    if (err != ESP_OK) {
        return fail(u, tr(T_FW_BOOT_FAILED), esp_err_to_name(err));
    }
    u->finished = true;
    ESP_LOGI(TAG, "Version %s is in %s. It starts after the restart.", u->version, u->part->label);
    return ESP_OK;
}

void fwupdate_end(fwupdate_t *u)
{
    if (!u) {
        return;
    }
    // After a successful update the device restarts: keep the update active until then.
    const bool finished = u->finished;
    const bool wrote = u->head_done;
    if (u->ota_started) {
        esp_ota_abort(u->ota);
    }
    if (u->hash_started) {
        psa_hash_abort(&u->hash);
    }
    free(u);
    if (!finished) {
        release();
        if (wrote) {
            notify();
        }
    }
}

const char *fwupdate_error(const fwupdate_t *u)
{
    return u->error;
}

const char *fwupdate_version(const fwupdate_t *u)
{
    return u->version;
}

const char *fwupdate_slot(const fwupdate_t *u)
{
    return u->part->label;
}

/* Download */

static void set_status(fwupdate_state_t state, const char *text)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.state = state;
    strlcpy(s_status.text, text, sizeof(s_status.text));
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_progress(size_t done, size_t total)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.done = done;
    s_status.total = total;
    taskEXIT_CRITICAL(&s_status_lock);
}

void fwupdate_get_status(fwupdate_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Open the URL and follow the redirects. Return the HTTP status, or -1 if no connection.
static int open_url(esp_http_client_handle_t c, int64_t *length, char *buf)
{
    for (int i = 0; i <= DL_REDIRECTS; i++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Open: %s", esp_err_to_name(err));
            return -1;
        }
        *length = esp_http_client_fetch_headers(c);
        if (*length < 0) {
            return -1;
        }
        const int status = esp_http_client_get_status_code(c);
        if (!is_redirect(status) || i == DL_REDIRECTS) {
            return status;
        }
        // Read the body of the redirect answer, then go to the new address.
        while (esp_http_client_read(c, buf, DL_CHUNK) > 0) {
        }
        err = esp_http_client_set_redirection(c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Redirect: %s", esp_err_to_name(err));
            return status;
        }
    }
    return -1;
}

// Download the archive and update. Write the result to text. Return true when the new
// firmware is ready for the restart.
static bool download(const char *url, char *text, size_t text_len)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = DL_TIMEOUT_MS,
        .buffer_size = DL_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char *buf = malloc(DL_CHUNK);
    if (!c || !buf) {
        strlcpy(text, c ? tr(T_MSG_NO_MEMORY) : tr(T_FW_BAD_URL), text_len);
        if (c) {
            esp_http_client_cleanup(c);
        }
        free(buf);
        release();
        return false;
    }

    bool ok = false;
    int64_t length = 0;
    const int status = open_url(c, &length, buf);
    fwupdate_t *u = NULL;
    if (status < 0) {
        snprintf(text, text_len, tr(T_FW_NO_CONNECTION));
    } else if (status != 200) {
        snprintf(text, text_len, tr(T_FW_HTTP_STATUS), status);
    } else if ((u = create()) == NULL) {
        snprintf(text, text_len, "%s", s_begin_error);
    } else {
        const size_t total = length > 0 ? (size_t)length : 0;
        size_t done = 0;
        int waits = 0;
        bool broken = false;
        set_progress(0, total);
        for (;;) {
            const int n = esp_http_client_read(c, buf, DL_CHUNK);
            if (n == -ESP_ERR_HTTP_EAGAIN && ++waits < DL_MAX_WAITS) {
                continue;
            }
            if (n < 0) {
                snprintf(text, text_len, tr(T_FW_LINK_LOST));
                broken = true;
                break;
            }
            if (n == 0) {
                break;
            }
            waits = 0;
            if (fwupdate_write(u, buf, (size_t)n) != ESP_OK) {
                snprintf(text, text_len, "%s", fwupdate_error(u));
                broken = true;
                break;
            }
            done += (size_t)n;
            set_progress(done, total);
        }
        if (broken) {
            // The text is set.
        } else if (!esp_http_client_is_complete_data_received(c)) {
            snprintf(text, text_len, tr(T_FW_DOWNLOAD_CUT));
        } else if (fwupdate_finish(u) != ESP_OK) {
            snprintf(text, text_len, "%s", fwupdate_error(u));
        } else {
            snprintf(text, text_len, tr(T_MSG_OTA_DONE),
                     fwupdate_version(u), fwupdate_slot(u));
            ok = true;
        }
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(buf);
    if (u) {
        fwupdate_end(u);    // it also gives back the right to update, if the update failed
    } else {
        release();
    }
    return ok;
}

static void download_task(void *arg)
{
    char *url = arg;
    char text[sizeof(s_status.text)];
    ESP_LOGI(TAG, "Download %s", url);
    const bool ok = download(url, text, sizeof(text));
    free(url);
    ESP_LOGI(TAG, "%s", text);
    set_status(ok ? FWUPDATE_DONE : FWUPDATE_FAILED, text);
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(3000));    // the page reads the result in this time
        esp_restart();
    }
    vTaskDelete(NULL);
}

esp_err_t fwupdate_download(const char *url)
{
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!reserve()) {
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(url);
    if (!copy) {
        release();
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = FWUPDATE_RUNNING;
    strlcpy(s_status.text, tr(T_FW_CONNECTING), sizeof(s_status.text));
    taskEXIT_CRITICAL(&s_status_lock);
    if (xTaskCreate(download_task, "fwupdate", DL_STACK, copy, 4, NULL) != pdPASS) {
        free(copy);
        release();
        set_status(FWUPDATE_FAILED, tr(T_FW_NO_MEMORY_DOWNLOAD));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Check an archive on a server */

// Read exactly len bytes of the body. Return false on an error or at the end of the body.
static bool read_exact(esp_http_client_handle_t c, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        const int n = esp_http_client_read(c, (char *)buf + got, (int)(len - got));
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void copy_field(const cJSON *root, const char *key, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    out[0] = '\0';
    if (cJSON_IsString(item)) {
        strlcpy(out, item->valuestring, len);
    }
}

esp_err_t fwupdate_check(const char *url, fwupdate_manifest_t *out, char *text, size_t text_len)
{
    memset(out, 0, sizeof(*out));
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = DL_TIMEOUT_MS,
        .buffer_size = DL_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    uint8_t *buf = malloc(DL_CHUNK);
    if (!c || !buf) {
        strlcpy(text, c ? tr(T_MSG_NO_MEMORY) : tr(T_FW_BAD_URL), text_len);
        if (c) {
            esp_http_client_cleanup(c);
        }
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    int64_t length = 0;
    size_t size = 0;
    const int status = open_url(c, &length, (char *)buf);
    if (status < 0) {
        snprintf(text, text_len, tr(T_FW_NO_CONNECTION));
    } else if (status != 200) {
        snprintf(text, text_len, tr(T_FW_HTTP_STATUS), status);
    } else if (!read_exact(c, buf, TAR_BLOCK)) {
        snprintf(text, text_len, tr(T_FW_NO_ARCHIVE));
    } else if (memcmp(buf + 257, "ustar", 5) != 0) {
        snprintf(text, text_len, tr(T_FW_URL_NOT_TAR));
    } else if (strncmp((const char *)buf, MANIFEST_NAME, 100) != 0 || !parse_octal(buf + 124, 12, &size) ||
               size == 0 || size >= MANIFEST_MAX) {
        snprintf(text, text_len, tr(T_FW_MANIFEST_NOT_FIRST));
    } else if (!read_exact(c, buf, size)) {
        snprintf(text, text_len, tr(T_FW_NO_MANIFEST_ANSWER));
    } else {
        cJSON *root = cJSON_ParseWithLength((const char *)buf, size);
        if (!root) {
            snprintf(text, text_len, tr(T_FW_MANIFEST_BROKEN));
        } else {
            copy_field(root, "project", out->project, sizeof(out->project));
            copy_field(root, "version", out->version, sizeof(out->version));
            copy_field(root, "chip", out->chip, sizeof(out->chip));
            copy_field(root, "git", out->git, sizeof(out->git));
            copy_field(root, "built", out->built, sizeof(out->built));
            copy_field(root, "elf_sha256", out->elf_sha256, sizeof(out->elf_sha256));
            cJSON_Delete(root);
            if (out->version[0]) {
                err = ESP_OK;
                ESP_LOGI(TAG, "Check: %s %s (%s), built %s", out->project, out->version, out->git, out->built);
            } else {
                snprintf(text, text_len, tr(T_FW_NO_VERSION));
            }
        }
    }
    // Only the manifest: stop the download of the image.
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(buf);
    return err;
}
