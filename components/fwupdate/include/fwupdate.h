// Firmware update from a release archive.
//
// The archive is a tar file with two entries, in this order:
//   manifest.json      {"project", "version", "git", "built", "elf_sha256", "chip", "file", "size", "sha256"}
//   <file>             the application image (build/<project>.bin)
// The update reads the archive as a stream. Before it erases the flash, it checks the manifest
// and the start of the image (chip, project, version). While it writes, it calculates SHA-256.
// The new slot becomes the boot slot only when the SHA-256 is the same as in the manifest.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fwupdate fwupdate_t;

// Called when an update starts or stops (for example, to show it on the screen).
typedef void (*fwupdate_notify_fn_t)(void);

void fwupdate_set_notify(fwupdate_notify_fn_t fn);

// Start an update. Only one update can run at a time.
// Return NULL if an update runs already, or if there is no memory or no OTA slot.
// The reason is in fwupdate_begin_error().
fwupdate_t *fwupdate_begin(void);
const char *fwupdate_begin_error(void);

// Give the next part of the archive.
esp_err_t fwupdate_write(fwupdate_t *u, const void *data, size_t len);

// Make sure that the archive had the image with the correct SHA-256, then select the new slot
// for the next start. The caller restarts the device.
esp_err_t fwupdate_finish(fwupdate_t *u);

// Stop the update (if it is not finished) and release the memory.
void fwupdate_end(fwupdate_t *u);

// The reason of the last error, for the user (Ukrainian).
const char *fwupdate_error(const fwupdate_t *u);

// The version from the manifest, or "" before the manifest.
const char *fwupdate_version(const fwupdate_t *u);

// The label of the slot that gets the new firmware.
const char *fwupdate_slot(const fwupdate_t *u);

// True while an update (upload or download) runs.
bool fwupdate_active(void);

/* Download from a URL */

typedef enum {
    FWUPDATE_IDLE,
    FWUPDATE_RUNNING,
    FWUPDATE_DONE,      // the device restarts
    FWUPDATE_FAILED,
} fwupdate_state_t;

typedef struct {
    fwupdate_state_t state;
    size_t done;        // bytes received
    size_t total;       // archive size, 0 = not known
    char text[200];     // the result or the error, for the user
} fwupdate_status_t;

// Start the download of an archive in a separate task (http:// or https://).
// Return ESP_ERR_INVALID_STATE if an update runs already.
esp_err_t fwupdate_download(const char *url);

void fwupdate_get_status(fwupdate_status_t *out);

/* Check an archive on a server */

// The manifest of an archive. Missing fields are "" (an archive of an older build has no git, built,
// elf_sha256).
typedef struct {
    char project[32];
    char version[32];
    char chip[16];
    char git[24];           // "754311d" or "754311d-dirty"
    char built[24];         // the local time of the build computer, "2026-11-14 19:34"
    char elf_sha256[68];    // 64 hex digits
} fwupdate_manifest_t;

// Read only manifest.json of the archive at url (the first entry), without the image.
// On an error return ESP_FAIL and write the reason for the user (Ukrainian) to text.
esp_err_t fwupdate_check(const char *url, fwupdate_manifest_t *out, char *text, size_t text_len);

#ifdef __cplusplus
}
#endif
