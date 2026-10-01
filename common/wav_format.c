#include "wav_format.h"
#include <string.h>

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_EXTENSIBLE 0xfffe

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* KSDATAFORMAT_SUBTYPE_PCM: 00000001-0000-0010-8000-00aa00389b71 */
static const uint8_t pcm_guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                     0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

wav_result_t wav_parse(FILE *f, uint64_t file_size, wav_info_t *out)
{
    uint8_t h[40];
    memset(out, 0, sizeof *out);
    if (file_size < 12 || fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12 ||
        memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return WAV_NOT_RIFF;
    bool have_fmt = false;
    uint16_t format = 0, block_align = 0;
    uint64_t pos = 12;
    /* The RIFF size may be stale in streamed files; bound chunks by the file. */
    while (pos + 8 <= file_size) {
        if (fseek(f, (long)pos, SEEK_SET) || fread(h, 1, 8, f) != 8) return WAV_BAD_CHUNKS;
        uint32_t size = le32(h + 4);
        uint64_t body = pos + 8;
        if (!memcmp(h, "fmt ", 4)) {
            if (have_fmt || size < 16 || body + size > file_size) return WAV_BAD_CHUNKS;
            size_t n = size < sizeof h ? size : sizeof h;
            if (fread(h, 1, n, f) != n) return WAV_BAD_CHUNKS;
            format = le16(h);
            out->channels = le16(h + 2);
            out->sample_rate = le32(h + 4);
            block_align = le16(h + 12);
            out->bits_per_sample = le16(h + 14);
            if (format == WAVE_FORMAT_EXTENSIBLE) {
                if (size < 40) return WAV_BAD_CHUNKS;
                format = memcmp(h + 24, pcm_guid, 16) ? 0 : WAVE_FORMAT_PCM;
            }
            have_fmt = true;
        } else if (!memcmp(h, "data", 4)) {
            if (!have_fmt) return WAV_BAD_CHUNKS;
            if (format != WAVE_FORMAT_PCM || out->bits_per_sample != 16 ||
                (out->channels != 1 && out->channels != 2) ||
                block_align != out->channels * 2 ||
                out->sample_rate < 8000 || out->sample_rate > 48000) return WAV_UNSUPPORTED;
            uint64_t available = file_size - body;
            uint64_t bytes = size < available ? size : available;
            bytes -= bytes % block_align;
            if (!bytes || bytes > UINT32_MAX) return WAV_BAD_CHUNKS;
            out->data_offset = (uint32_t)body;
            out->data_bytes = (uint32_t)bytes;
            return WAV_OK;
        }
        pos = body + size + (size & 1);   /* chunks are word aligned */
    }
    return WAV_BAD_CHUNKS;
}

const char *wav_result_text(wav_result_t r)
{
    switch (r) {
    case WAV_OK:          return "ok";
    case WAV_NOT_RIFF:    return "not a RIFF/WAVE file";
    case WAV_BAD_CHUNKS:  return "missing or truncated fmt/data chunk";
    case WAV_UNSUPPORTED: return "unsupported format (need 16-bit PCM, mono/stereo, 8-48 kHz)";
    }
    return "unknown";
}
