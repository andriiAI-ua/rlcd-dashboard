// Uploaded sound files (sound_files.c) for the player (audio.c). Private to the audio component.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Find the "sounds" partition. Call once before the other functions.
void sound_files_init(void);

// Lock the files for the playback. Return false while an upload runs.
bool sound_files_lock(void);
void sound_files_unlock(void);

// Read the header of a slot. Return false if the slot has no valid file.
bool sound_files_header(int slot, unsigned *rate, size_t *samples);

// Read mono 16-bit samples of a slot, from the sample "first".
esp_err_t sound_files_read(int slot, size_t first, int16_t *buf, size_t count);
