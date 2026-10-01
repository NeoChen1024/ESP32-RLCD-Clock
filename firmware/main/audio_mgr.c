#include "audio_mgr.h"
#include "sensors.h"
#include "storage_mgr.h"
#include "wav_format.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define PIN_MCLK 16
#define PIN_BCLK 9
#define PIN_WS   45
#define PIN_DOUT 8
#define PIN_PA   46
#define PA_GAIN_DB 6            /* vendor board configuration */

#define STREAM_BYTES (512 * 1024) /* about 3 s of 44.1 kHz stereo */
#define READ_CHUNK   (8 * 1024)
#define WRITE_CHUNK  4096

typedef enum { CMD_PLAY, CMD_STOP } cmd_op_t;
typedef struct { cmd_op_t op; bool loop; char volume[8]; char relative[STORAGE_REL_MAX]; } cmd_t;

static esp_codec_dev_handle_t s_dev;
static QueueHandle_t s_cmd;
static StreamBufferHandle_t s_stream;
static SemaphoreHandle_t s_writer_idle;
static TaskHandle_t s_writer;
static uint8_t *s_read_buf;

/* Reader-owned file; touched only with the storage mutex held. */
static FILE *s_file;
static char s_file_volume[8], s_file_relative[STORAGE_REL_MAX];

/* Status shared with CLI/HTTP readers. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_status_t s_status;
static uint32_t s_byte_rate;
static uint64_t s_bytes_written;
static volatile bool s_writer_active, s_stop_writer, s_reader_done;
static int s_config_volume = AUDIO_DEFAULT_VOLUME;

static void set_error(const char *text)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_status.last_error, sizeof s_status.last_error, "%s", text);
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "%s", text);
}

/* ---- writer: stream buffer -> codec ---- */

static void writer_task(void *arg)
{
    (void)arg;
    static uint8_t buf[WRITE_CHUNK];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = s_status.channels,
            .sample_rate = s_status.sample_rate,
        };
        int volume;
        taskENTER_CRITICAL(&s_mux);
        volume = s_status.volume;
        taskEXIT_CRITICAL(&s_mux);
        bool opened = esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK;
        if (opened) {
            esp_codec_dev_set_out_vol(s_dev, volume);
            int applied = volume;
            while (!s_stop_writer) {
                size_t n = xStreamBufferReceive(s_stream, buf, sizeof buf, pdMS_TO_TICKS(50));
                if (!n) {
                    if (s_reader_done && xStreamBufferIsEmpty(s_stream)) break;
                    taskENTER_CRITICAL(&s_mux);
                    s_status.underruns++;
                    taskEXIT_CRITICAL(&s_mux);
                    continue;
                }
                if (esp_codec_dev_write(s_dev, buf, (int)n) != ESP_CODEC_DEV_OK) {
                    set_error("codec write failed");
                    break;
                }
                taskENTER_CRITICAL(&s_mux);
                s_bytes_written += n;
                s_status.elapsed_ms = (uint32_t)(s_bytes_written * 1000 / s_byte_rate);
                s_status.position_ms = s_status.duration_ms ? s_status.elapsed_ms % s_status.duration_ms : 0;
                s_status.loops = s_status.duration_ms ? s_status.elapsed_ms / s_status.duration_ms : 0;
                volume = s_status.volume;
                taskEXIT_CRITICAL(&s_mux);
                if (volume != applied) { esp_codec_dev_set_out_vol(s_dev, volume); applied = volume; }
            }
            esp_codec_dev_close(s_dev);   /* also disables the PA */
        } else {
            set_error("codec open failed");
        }
        taskENTER_CRITICAL(&s_mux);
        s_status.state = AUDIO_IDLE;
        taskEXIT_CRITICAL(&s_mux);
        s_writer_active = false;
        xSemaphoreGive(s_writer_idle);
    }
}

/* ---- reader: file -> stream buffer ---- */

static void close_file_locked(void)
{
    if (s_file) fclose(s_file);
    s_file = NULL;
    s_file_volume[0] = s_file_relative[0] = 0;
}

static void stop_writer(void)
{
    if (s_writer_active) {
        s_stop_writer = true;
        if (xSemaphoreTake(s_writer_idle, pdMS_TO_TICKS(2000)) != pdTRUE) ESP_LOGE(TAG, "writer did not stop");
        s_stop_writer = false;
    }
    xSemaphoreTake(s_writer_idle, 0);   /* clear a give from a natural track end */
    xStreamBufferReset(s_stream);       /* writer is idle, so nothing is blocked on it */
}

static bool open_file(const cmd_t *c, wav_info_t *out)
{
    char path[STORAGE_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", storage_root(c->volume), c->relative);
    if (!storage_lock(5000)) { set_error("storage busy"); return false; }
    bool ok = false;
    if (!storage_mounted_locked(c->volume)) set_error("volume is not mounted");
    else if (!(s_file = fopen(path, "rb"))) set_error("cannot open file");
    else {
        struct stat st;
        wav_info_t info;
        wav_result_t r = fstat(fileno(s_file), &st) ? WAV_NOT_RIFF
                       : wav_parse(s_file, (uint64_t)st.st_size, &info);
        if (r != WAV_OK) set_error(wav_result_text(r));
        else if (fseek(s_file, (long)info.data_offset, SEEK_SET)) set_error("seek failed");
        else {
            snprintf(s_file_volume, sizeof s_file_volume, "%s", c->volume);
            snprintf(s_file_relative, sizeof s_file_relative, "%s", c->relative);
            *out = info;
            s_byte_rate = info.sample_rate * info.channels * 2;
            uint32_t duration_ms = (uint32_t)((uint64_t)info.data_bytes * 1000 / s_byte_rate);
            taskENTER_CRITICAL(&s_mux);
            s_status.state = AUDIO_PLAYING;
            snprintf(s_status.volume_name, sizeof s_status.volume_name, "%s", c->volume);
            snprintf(s_status.relative, sizeof s_status.relative, "%s", c->relative);
            s_status.sample_rate = info.sample_rate;
            s_status.channels = info.channels;
            s_status.position_ms = s_status.elapsed_ms = s_status.loops = 0;
            s_status.underruns = 0;
            s_status.last_error[0] = 0;
            s_status.duration_ms = duration_ms;
            /* A file at least as long as the limit plays once, uncut. */
            s_status.loop = c->loop && duration_ms < AUDIO_LOOP_LIMIT_MS;
            s_bytes_written = 0;
            taskEXIT_CRITICAL(&s_mux);
            ok = true;
        }
        if (!ok) close_file_locked();
    }
    storage_unlock();
    return ok;
}

static void reader_task(void *arg)
{
    (void)arg;
    bool reading = false;
    uint32_t remaining = 0;   /* bytes left in this pass */
    uint64_t budget = 0;      /* bytes left before the loop limit */
    wav_info_t info = {0};
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd, &c, reading ? 0 : portMAX_DELAY) == pdTRUE) {
            stop_writer();
            if (storage_lock(5000)) { close_file_locked(); storage_unlock(); }
            reading = false;
            if (c.op == CMD_PLAY && open_file(&c, &info)) {
                remaining = info.data_bytes;
                budget = s_status.loop ? (uint64_t)AUDIO_LOOP_LIMIT_MS / 1000 * s_byte_rate : UINT64_MAX;
                ESP_LOGI(TAG, "playing %s/%s (%u Hz, %u ch%s)", c.volume, c.relative,
                         (unsigned)info.sample_rate, info.channels, s_status.loop ? ", loop" : "");
                reading = true;
                s_reader_done = false;
                s_writer_active = true;
                xTaskNotifyGive(s_writer);
            }
            continue;
        }
        if (!s_writer_active) {             /* codec failed to open or write */
            if (storage_lock(5000)) { close_file_locked(); storage_unlock(); }
            reading = false;
            continue;
        }
        if (xStreamBufferSpacesAvailable(s_stream) < READ_CHUNK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!storage_lock(100)) continue;   /* the stream buffer covers short waits */
        size_t n = 0, want = remaining < READ_CHUNK ? remaining : READ_CHUNK;
        if (want > budget) want = (size_t)budget;
        if (s_file && want) {
            n = fread(s_read_buf, 1, want, s_file);
            if (!n) set_error(ferror(s_file) ? "read error" : "file shorter than its header");
        } else if (!s_file && remaining) {
            set_error("file released by storage");
        }
        n -= n % (info.channels * 2);         /* whole frames only */
        remaining = n ? remaining - (uint32_t)n : 0;
        budget -= n;
        if (!remaining && budget && n && s_file && s_status.loop) {
            /* Seamless repeat: rewind to the first sample of the data chunk. */
            if (fseek(s_file, (long)info.data_offset, SEEK_SET)) set_error("seek failed");
            else remaining = info.data_bytes;
        }
        if (remaining && !budget) {
            ESP_LOGI(TAG, "loop limit reached");
            remaining = 0;
        }
        if (!remaining) close_file_locked();
        storage_unlock();
        if (n) xStreamBufferSend(s_stream, s_read_buf, n, portMAX_DELAY);
        if (!remaining) { reading = false; s_reader_done = true; }
    }
}

/* ---- public API ---- */

bool audio_mgr_start(void)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;     /* underruns play silence, not stale DMA data */
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 480;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS,
                      .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED },
    };
    if (i2s_new_channel(&chan, &tx, NULL) != ESP_OK || i2s_channel_init_std_mode(tx, &std) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return false;
    }
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR,
                                      .bus_handle = sensors_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl_if = i2c_cfg.bus_handle ? audio_codec_new_i2c_ctrl(&i2c_cfg) : NULL;
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_PA, .use_mclk = true,
        .hw_gain.pa_gain = PA_GAIN_DB,
    };
    const audio_codec_if_t *codec_if = ctrl_if && gpio_if ? es8311_codec_new(&es) : NULL;
    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if };
    s_dev = codec_if && data_if ? esp_codec_dev_new(&dev_cfg) : NULL;
    if (!s_dev) { ESP_LOGE(TAG, "ES8311 init failed"); return false; }
    /* The library default spans -50..0 dB, leaving mid settings very quiet. */
    esp_codec_dev_vol_map_t map[] = { {.vol = 1, .db_value = -40.0f}, {.vol = 100, .db_value = 0.0f} };
    esp_codec_dev_vol_curve_t curve = { .vol_map = map, .count = 2 };
    esp_codec_dev_set_vol_curve(s_dev, &curve);

    s_stream = xStreamBufferCreateWithCaps(STREAM_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_read_buf = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM);
    s_cmd = xQueueCreate(2, sizeof(cmd_t));
    s_writer_idle = xSemaphoreCreateBinary();
    if (!s_stream || !s_read_buf || !s_cmd || !s_writer_idle) { ESP_LOGE(TAG, "out of memory"); return false; }
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.volume_override) s_status.volume = s_config_volume;
    taskEXIT_CRITICAL(&s_mux);
    /* The writer feeds DMA, so it outranks the reader and the display. */
    if (xTaskCreate(writer_task, "audio_out", 4096, NULL, 7, &s_writer) != pdPASS ||
        xTaskCreate(reader_task, "audio_in", 4096, NULL, 5, NULL) != pdPASS) return false;
    s_status.available = true;
    ESP_LOGI(TAG, "ES8311 ready");
    return true;
}

bool audio_mgr_play(const char *volume, const char *relative, bool loop)
{
    if (!s_status.available || !volume || !relative ||
        (strcmp(volume, "sd") && strcmp(volume, "flash")) ||
        strncmp(relative, "sounds/", 7) || !storage_file_allowed(relative)) return false;
    cmd_t c = { .op = CMD_PLAY, .loop = loop };
    snprintf(c.volume, sizeof c.volume, "%s", volume);
    snprintf(c.relative, sizeof c.relative, "%s", relative);
    return xQueueSend(s_cmd, &c, 0) == pdTRUE;
}

void audio_mgr_stop(void)
{
    if (!s_status.available) return;
    cmd_t c = { .op = CMD_STOP };
    xQueueSend(s_cmd, &c, pdMS_TO_TICKS(100));
}

static int clamp_volume(int volume) { return volume < 0 ? 0 : volume > 100 ? 100 : volume; }

void audio_mgr_set_config_volume(int volume)
{
    taskENTER_CRITICAL(&s_mux);
    s_config_volume = clamp_volume(volume);
    if (!s_status.volume_override) s_status.volume = s_config_volume;
    taskEXIT_CRITICAL(&s_mux);
}

void audio_mgr_set_volume(int volume)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.volume = clamp_volume(volume);
    s_status.volume_override = true;
    taskEXIT_CRITICAL(&s_mux);
}

void audio_mgr_reset_volume(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.volume_override = false;
    s_status.volume = s_config_volume;
    taskEXIT_CRITICAL(&s_mux);
}

void audio_mgr_status(audio_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

void audio_mgr_release_locked(const char *volume, const char *relative)
{
    if (!s_file || strcmp(volume, s_file_volume) || (relative && strcmp(relative, s_file_relative))) return;
    ESP_LOGI(TAG, "releasing %s/%s for a storage change", s_file_volume, s_file_relative);
    close_file_locked();   /* the reader stops; buffered audio drains */
}
