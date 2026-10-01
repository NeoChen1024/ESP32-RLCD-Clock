#ifndef RLCD_AUDIO_SOURCE_H
#define RLCD_AUDIO_SOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "audio_io.h"
#include "wav_format.h"

/*
 * Decoded 16-bit PCM from a managed sound file, shared by the player, upload
 * validation and host tests. WAV must be 16-bit PCM; FLAC (native container,
 * dr_flac) may use any bit depth and is converted to 16-bit. Both need 1 or
 * 2 channels at 8..48 kHz, the ES8311/I2S range used here.
 */

typedef enum { AUDIO_FORMAT_UNKNOWN = 0, AUDIO_FORMAT_WAV, AUDIO_FORMAT_FLAC } audio_format_t;

typedef enum {
    AUDIO_SRC_OK = 0,
    AUDIO_SRC_INVALID,       /* not a readable WAV/FLAC stream */
    AUDIO_SRC_UNSUPPORTED,   /* readable, but outside the playable formats */
    AUDIO_SRC_NO_MEMORY,
} audio_src_result_t;

typedef struct {
    audio_format_t format;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t source_bits;    /* bits per sample in the file */
    uint64_t total_frames;   /* 0 when a FLAC stream does not record it */
    bool error;              /* a read failed or the stream was corrupt */
    /* private */
    audio_io_t io;
    wav_info_t wav;
    uint64_t wav_frames_left;
    void *flac;
} audio_source_t;

/* By lower-case extension: ".wav" or ".flac". */
audio_format_t audio_format_from_name(const char *name);
audio_src_result_t audio_source_open(audio_source_t *s, audio_format_t format, const audio_io_t *io);
/* Interleaved frames; returns fewer than requested only at the end of the
 * stream or on error (then s->error is set). */
size_t audio_source_read(audio_source_t *s, int16_t *out, size_t frames);
/* Back to the first frame, for seamless loops. */
bool audio_source_rewind(audio_source_t *s);
void audio_source_close(audio_source_t *s);
const char *audio_src_text(audio_src_result_t r);

/* Upload validation: open and decode the start of the stream. */
audio_src_result_t audio_probe(const char *name, FILE *f, uint64_t size);
#endif
