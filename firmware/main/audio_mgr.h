#ifndef RLCD_AUDIO_MGR_H
#define RLCD_AUDIO_MGR_H
#include <stdbool.h>
#include <stdint.h>
#include "storage_files.h"

/*
 * WAV playback on the ES8311 DAC + speaker PA (I2S0 TX, MCLK 16, BCLK 9,
 * WS 45, DOUT 8, PA enable GPIO 46; control on the shared I2C bus).
 *
 * A reader task takes the storage mutex only around each chunk read and
 * fills a PSRAM stream buffer; a writer task drains it to the codec. The
 * PA is enabled only while a file is open. Storage owners that unmount,
 * format, delete or replace a file must call audio_mgr_release_locked()
 * first so the player never holds a file across that change.
 */

typedef enum { AUDIO_IDLE = 0, AUDIO_PLAYING } audio_state_t;

typedef struct {
    bool available;             /* codec initialized */
    audio_state_t state;
    int volume;                 /* 0..100, effective */
    bool volume_override;       /* CLI value in effect instead of config */
    char volume_name[8];
    char relative[STORAGE_REL_MAX];
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t position_ms;       /* written to the codec */
    uint32_t duration_ms;
    uint32_t underruns;         /* writer waited on an empty buffer */
    char last_error[64];
} audio_status_t;

bool audio_mgr_start(void);
/* Queue playback of a managed sounds/<name>.wav file; replaces any current one.
 * Returns false if the request is malformed or the queue is full; open
 * and format errors are reported through last_error. */
bool audio_mgr_play(const char *volume, const char *relative);
void audio_mgr_stop(void);
/* Volume 0..100: 0 mutes, 1..100 map linearly to -40..0 dB before the
 * PA-gain compensation. The config value applies unless a RAM-only CLI
 * override is set; reset returns to the config value. */
#define AUDIO_DEFAULT_VOLUME 80
void audio_mgr_set_config_volume(int volume);
void audio_mgr_set_volume(int volume);
void audio_mgr_reset_volume(void);
void audio_mgr_status(audio_status_t *out);
/* Storage owner holds the mutex. relative NULL releases any file on volume. */
void audio_mgr_release_locked(const char *volume, const char *relative);
#endif
