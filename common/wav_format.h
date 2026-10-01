#ifndef RLCD_WAV_FORMAT_H
#define RLCD_WAV_FORMAT_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "audio_io.h"

/*
 * RIFF/WAVE header parsing shared by upload validation, the player and host
 * tests. Playable files are linear PCM (WAVE_FORMAT_PCM, or EXTENSIBLE with
 * the PCM subformat), 16-bit, mono or stereo, at 8..48 kHz. Unknown chunks
 * are skipped; the data chunk must fit inside the file and hold whole frames.
 */

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_offset;   /* file offset of the first sample */
    uint32_t data_bytes;    /* whole frames only */
} wav_info_t;

typedef enum {
    WAV_OK = 0,
    WAV_NOT_RIFF,           /* not a RIFF/WAVE container, or truncated header */
    WAV_BAD_CHUNKS,         /* fmt/data missing, out of order or past the file end */
    WAV_UNSUPPORTED,        /* valid WAVE, but not a playable PCM format */
} wav_result_t;

wav_result_t wav_parse_io(const audio_io_t *io, wav_info_t *out);
/* f is positioned anywhere; file_size is its length in bytes. */
wav_result_t wav_parse(FILE *f, uint64_t file_size, wav_info_t *out);
const char *wav_result_text(wav_result_t r);
#endif
