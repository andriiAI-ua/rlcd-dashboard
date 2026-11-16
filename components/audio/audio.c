// Audio: ES8311 codec through esp_codec_dev (the same setup as the Waveshare audio example:
// I2S MCLK 16, BCLK 9, WS 45, DOUT 8; amplifier enable GPIO46; codec on the I2C bus at 0x18).
#include "audio.h"

#include <math.h>
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sound_files.h"

static const char *TAG = "audio";

#define SAMPLE_RATE      16000  // built-in sounds
#define CHUNK            256    // stereo frames for one write
#define TWO_PI           6.28318531f
#define FILE_MAX_SECONDS 60     // a longer file plays only to this time

typedef struct {
    audio_sound_t sound;
    int slot;
} request_t;

static esp_codec_dev_handle_t s_dev;
static QueueHandle_t s_queue;
static volatile int s_volume = 70;
static volatile bool s_playing;
static unsigned s_rate = SAMPLE_RATE;   // the sample rate of the open codec
static int16_t s_buf[CHUNK * 2];

static const char *const IDS[AUDIO_SOUND_COUNT] = { "off", "siren", "warble", "beeps", "chime", "file" };

audio_sound_t audio_sound_from_id(const char *id, audio_sound_t def)
{
    for (int i = 0; id && i < AUDIO_SOUND_COUNT; i++) {
        if (strcmp(id, IDS[i]) == 0) {
            return (audio_sound_t)i;
        }
    }
    return def;
}

const char *audio_sound_id(audio_sound_t sound)
{
    return sound >= 0 && sound < AUDIO_SOUND_COUNT ? IDS[sound] : IDS[AUDIO_SOUND_OFF];
}

// Play a tone that goes from f0 to f1 (Hz) in ms. fade_in_ms and fade_out_ms prevent clicks.
static void tone(float *phase, float f0, float f1, int ms, int fade_in_ms, int fade_out_ms)
{
    const int total = SAMPLE_RATE * ms / 1000;
    const int fade_in = SAMPLE_RATE * fade_in_ms / 1000;
    const int fade_out = SAMPLE_RATE * fade_out_ms / 1000;
    int done = 0;
    while (done < total) {
        const int n = total - done < CHUNK ? total - done : CHUNK;
        for (int i = 0; i < n; i++) {
            const int k = done + i;
            const float f = f0 + (f1 - f0) * (float)k / (float)total;
            *phase += TWO_PI * f / SAMPLE_RATE;
            if (*phase > TWO_PI) {
                *phase -= TWO_PI;
            }
            // A sine with some third harmonic: it is louder on a small speaker.
            const float v = (sinf(*phase) + 0.3f * sinf(3.0f * *phase)) / 1.3f;
            float env = 1.0f;
            if (fade_in > 0 && k < fade_in) {
                env = (float)k / (float)fade_in;
            } else if (fade_out > 0 && k > total - fade_out) {
                env = (float)(total - k) / (float)fade_out;
            }
            const int16_t s = (int16_t)(v * env * 0.7f * 32767.0f);
            s_buf[2 * i] = s;
            s_buf[2 * i + 1] = s;
        }
        esp_codec_dev_write(s_dev, s_buf, n * 2 * (int)sizeof(int16_t));
        done += n;
    }
}

static void silence(int ms)
{
    memset(s_buf, 0, sizeof(s_buf));
    for (int left = (int)s_rate * ms / 1000; left > 0; left -= CHUNK) {
        const int n = left < CHUNK ? left : CHUNK;
        esp_codec_dev_write(s_dev, s_buf, n * 2 * (int)sizeof(int16_t));
    }
}

static void play_built_in(audio_sound_t sound)
{
    float phase = 0;
    switch (sound) {
    case AUDIO_SOUND_SIREN:
        // The tone goes up and down three times.
        for (int i = 0; i < 3; i++) {
            tone(&phase, 600, 1300, 650, i == 0 ? 30 : 0, 0);
            tone(&phase, 1300, 600, 650, 0, i == 2 ? 60 : 0);
        }
        break;
    case AUDIO_SOUND_WARBLE:
        // Two tones change fast. The phase continues, so there are no clicks.
        for (int i = 0; i < 10; i++) {
            tone(&phase, 950, 950, 150, i == 0 ? 20 : 0, 0);
            tone(&phase, 1250, 1250, 150, 0, i == 9 ? 40 : 0);
        }
        break;
    case AUDIO_SOUND_BEEPS:
        for (int i = 0; i < 5; i++) {
            tone(&phase, 1200, 1200, 140, 10, 30);
            silence(110);
        }
        break;
    case AUDIO_SOUND_CHIME:
        // Three calm tones down: G5, E5, C5.
        tone(&phase, 784, 784, 350, 15, 40);
        silence(80);
        tone(&phase, 659, 659, 350, 15, 40);
        silence(80);
        tone(&phase, 523, 523, 650, 15, 200);
        break;
    default:
        break;
    }
}

static void play_file(int slot, size_t samples)
{
    static int16_t mono[CHUNK];
    const size_t limit = (size_t)s_rate * FILE_MAX_SECONDS;
    if (samples > limit) {
        samples = limit;
    }
    for (size_t done = 0; done < samples;) {
        const size_t n = samples - done < CHUNK ? samples - done : CHUNK;
        if (sound_files_read(slot, done, mono, n) != ESP_OK) {
            ESP_LOGW(TAG, "Cannot read slot %d", slot);
            break;
        }
        for (size_t i = 0; i < n; i++) {
            s_buf[2 * i] = mono[i];
            s_buf[2 * i + 1] = mono[i];
        }
        esp_codec_dev_write(s_dev, s_buf, (int)(n * 2 * sizeof(int16_t)));
        done += n;
    }
}

static void play(const request_t *r)
{
    unsigned rate = SAMPLE_RATE;
    size_t samples = 0;
    const bool file = r->sound == AUDIO_SOUND_FILE;
    if (r->sound == AUDIO_SOUND_OFF) {
        return;
    }
    if (file) {
        if (!sound_files_lock()) {
            ESP_LOGW(TAG, "An upload runs: no sound");
            return;
        }
        if (!sound_files_header(r->slot, &rate, &samples)) {
            sound_files_unlock();
            ESP_LOGW(TAG, "Slot %d has no file: no sound", r->slot);
            return;
        }
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Cannot open the codec at %u Hz", rate);
        if (file) {
            sound_files_unlock();
        }
        return;
    }
    s_rate = rate;
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    if (file) {
        play_file(r->slot, samples);
    } else {
        play_built_in(r->sound);
    }
    silence(120);  // let the last samples out before the amplifier goes off
    esp_codec_dev_close(s_dev);
    if (file) {
        sound_files_unlock();
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    request_t r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY) == pdTRUE) {
            s_playing = true;
            play(&r);
            s_playing = false;
        }
    }
}

// The board has an ES7210 for two microphones. The firmware does not use them: put the chip in power-down
// (the register sequence of es7210_stop() in esp_codec_dev), else it uses current also in sleep.
#define ES7210_ADDR 0x40

static void microphones_off(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, ES7210_ADDR, 50) != ESP_OK) {
        ESP_LOGI(TAG, "No ES7210 (microphones)");
        return;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES7210_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return;
    }
    static const uint8_t OFF[][2] = {
        { 0x47, 0xFF }, { 0x48, 0xFF }, { 0x49, 0xFF }, { 0x4A, 0xFF }, { 0x4B, 0xFF },
        { 0x4C, 0xFF }, { 0x40, 0xC0 }, { 0x01, 0x7F }, { 0x06, 0x07 },
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(OFF) / sizeof(OFF[0]) && err == ESP_OK; i++) {
        err = i2c_master_transmit(dev, OFF[i], 2, 100);
    }
    i2c_master_bus_rm_device(dev);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ES7210 (microphones) is in power-down");
    } else {
        ESP_LOGW(TAG, "ES7210 power-down: %s", esp_err_to_name(err));
    }
}

esp_err_t audio_init(i2c_master_bus_handle_t bus)
{
    // The files do not need the codec: the page can show and change them also without it.
    sound_files_init();

    // In light sleep ESP-IDF isolates all pins. Keep the amplifier enable pin as it is (off).
    gpio_sleep_sel_dis(BOARD_PA_CTRL);

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, NULL), TAG, "I2S channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_SCLK,
            .ws = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "I2S standard mode");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_PA_CTRL,
        .pa_reverted = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f, .pa_gain = 6.0f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_ERR_NOT_FOUND, TAG, "no ES8311 codec");

    // es8311_codec_new() turns the codec on. Turn it off until the first sound: the register sequence of
    // es8311_suspend() in esp_codec_dev. Each sound starts it (es8311_start) and turns it off at the end.
    static const uint8_t ES8311_OFF[][2] = {
        { 0x32, 0x00 }, { 0x17, 0x00 }, { 0x0E, 0xFF }, { 0x12, 0x02 }, { 0x14, 0x00 },
        { 0x0D, 0xFA }, { 0x15, 0x00 }, { 0x02, 0x10 }, { 0x00, 0x00 }, { 0x00, 0x1F },
        { 0x01, 0x30 }, { 0x01, 0x00 }, { 0x45, 0x00 }, { 0x0D, 0xFC }, { 0x02, 0x00 },
    };
    for (size_t i = 0; i < sizeof(ES8311_OFF) / sizeof(ES8311_OFF[0]); i++) {
        codec_if->set_reg(codec_if, ES8311_OFF[i][0], ES8311_OFF[i][1]);
    }
    microphones_off(bus);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec device");

    s_queue = xQueueCreate(4, sizeof(request_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    xTaskCreate(audio_task, "audio", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Audio is ready");
    return ESP_OK;
}

void audio_set_volume(int volume)
{
    s_volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
}

void audio_play(audio_sound_t sound, int slot)
{
    const request_t r = { .sound = sound, .slot = slot };
    if (s_queue && sound != AUDIO_SOUND_OFF) {
        xQueueSend(s_queue, &r, 0);
    }
}

bool audio_busy(void)
{
    return s_playing || (s_queue && uxQueueMessagesWaiting(s_queue) > 0);
}
