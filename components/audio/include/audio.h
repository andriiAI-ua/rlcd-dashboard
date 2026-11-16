// Sounds through the ES8311 codec, the NS4150B amplifier and the 8 ohm speaker (SPK connector).
// Built-in sounds are made in code. Uploaded WAV files are in the "sounds" partition, one file for each
// slot. The codec and the amplifier are on only while a sound plays.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_SOUND_OFF,
    AUDIO_SOUND_SIREN,      // the tone goes up and down three times, about 4 s
    AUDIO_SOUND_WARBLE,     // two tones change fast, about 3 s
    AUDIO_SOUND_BEEPS,      // five short beeps
    AUDIO_SOUND_CHIME,      // three calm tones down
    AUDIO_SOUND_FILE,       // the uploaded file of a slot
    AUDIO_SOUND_COUNT,
} audio_sound_t;

// Uploaded files: one slot for each event (missile alert, drone alert, all clear).
#define AUDIO_SLOT_COUNT 3

// The sound for a settings value ("siren", "warble", "beeps", "chime", "file", "off"), and back.
// An unknown value gives def.
audio_sound_t audio_sound_from_id(const char *id, audio_sound_t def);
const char *audio_sound_id(audio_sound_t sound);

// Start the audio task. bus: the shared I2C bus (the codec is at 0x18).
// The uploaded files work also when the codec is not found.
esp_err_t audio_init(i2c_master_bus_handle_t bus);

// Set the volume, 0..100.
void audio_set_volume(int volume);

// Play a sound in the audio task. slot: the file slot for AUDIO_SOUND_FILE.
// The call does not wait. A new sound waits for the current one.
void audio_play(audio_sound_t sound, int slot);

// Return true while a sound plays (the device must not sleep then).
bool audio_busy(void);

/* Uploaded files */

typedef struct {
    bool valid;
    unsigned rate;          // samples per second
    unsigned tenths;        // length in 0.1 s
    char name[48];          // the name of the uploaded file
} audio_file_info_t;

// True when the firmware has the "sounds" partition (the partition table is new enough).
bool audio_files_available(void);

// The longest file in seconds at a sample rate (the slot keeps mono 16-bit samples).
unsigned audio_file_max_seconds(unsigned rate);

void audio_file_info(int slot, audio_file_info_t *out);

esp_err_t audio_file_delete(int slot);

// Upload a WAV file (PCM, 8 or 16 bits, mono or stereo, 8000..48000 Hz) as a stream.
// The slot keeps a mono 16-bit copy. The old file of the slot is gone after the start.
typedef struct audio_upload audio_upload_t;

// Return NULL on an error; the reason is in audio_upload_begin_error().
audio_upload_t *audio_upload_begin(int slot, const char *name);
const char *audio_upload_begin_error(void);
esp_err_t audio_upload_write(audio_upload_t *u, const void *data, size_t len);
esp_err_t audio_upload_finish(audio_upload_t *u);
// The reason of the last error, for the user (Ukrainian).
const char *audio_upload_error(const audio_upload_t *u);
void audio_upload_end(audio_upload_t *u);

#ifdef __cplusplus
}
#endif
