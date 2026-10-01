#include "audio_source.h"
#include <stdlib.h>
#include <string.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"

static bool playable(uint32_t rate, uint16_t channels)
{
    return rate >= 8000 && rate <= 48000 && (channels == 1 || channels == 2);
}

audio_format_t audio_format_from_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n > 4 && !strcmp(name + n - 4, ".wav")) return AUDIO_FORMAT_WAV;
    if (n > 5 && !strcmp(name + n - 5, ".flac")) return AUDIO_FORMAT_FLAC;
    return AUDIO_FORMAT_UNKNOWN;
}

/* ---- dr_flac callbacks: translate relative seeks into bounded absolute ones ---- */

typedef struct { audio_source_t *s; uint64_t pos; } flac_cursor_t;

static size_t flac_read(void *user, void *buf, size_t n)
{
    flac_cursor_t *c = user;
    size_t got = c->s->io.read(c->s->io.ctx, buf, n);
    c->pos += got;
    return got;
}

static drflac_bool32 flac_seek(void *user, int offset, drflac_seek_origin origin)
{
    flac_cursor_t *c = user;
    int64_t base = origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_CUR ? (int64_t)c->pos
                 : (int64_t)c->s->io.size;
    int64_t target = base + offset;
    /* dr_flac probes past the end while seeking; that must fail cleanly. */
    if (target < 0 || (uint64_t)target > c->s->io.size || !c->s->io.seek(c->s->io.ctx, (uint64_t)target))
        return DRFLAC_FALSE;
    c->pos = (uint64_t)target;
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_tell(void *user, drflac_int64 *cursor)
{
    *cursor = (drflac_int64)((flac_cursor_t *)user)->pos;
    return DRFLAC_TRUE;
}

/* dr_flac keeps the user pointer; the cursor lives with the decoder. */
typedef struct { drflac *decoder; flac_cursor_t cursor; } flac_state_t;

audio_src_result_t audio_source_open(audio_source_t *s, audio_format_t format, const audio_io_t *io)
{
    memset(s, 0, sizeof *s);
    s->format = format;
    s->io = *io;
    if (format == AUDIO_FORMAT_WAV) {
        wav_result_t r = wav_parse_io(&s->io, &s->wav);
        if (r != WAV_OK) return r == WAV_UNSUPPORTED ? AUDIO_SRC_UNSUPPORTED : AUDIO_SRC_INVALID;
        if (!s->io.seek(s->io.ctx, s->wav.data_offset)) return AUDIO_SRC_INVALID;
        s->sample_rate = s->wav.sample_rate;
        s->channels = s->wav.channels;
        s->source_bits = 16;
        s->total_frames = s->wav_frames_left = s->wav.data_bytes / (s->channels * 2u);
        return AUDIO_SRC_OK;
    }
    if (format != AUDIO_FORMAT_FLAC) return AUDIO_SRC_INVALID;
    flac_state_t *f = malloc(sizeof *f);
    if (!f) return AUDIO_SRC_NO_MEMORY;
    f->cursor = (flac_cursor_t){ .s = s, .pos = 0 };
    if (!s->io.seek(s->io.ctx, 0) ||
        !(f->decoder = drflac_open(flac_read, flac_seek, flac_tell, &f->cursor, NULL))) {
        free(f);
        return AUDIO_SRC_INVALID;
    }
    s->flac = f;
    s->sample_rate = f->decoder->sampleRate;
    s->channels = f->decoder->channels;
    s->source_bits = f->decoder->bitsPerSample;
    s->total_frames = f->decoder->totalPCMFrameCount;
    if (!playable(s->sample_rate, s->channels)) { audio_source_close(s); return AUDIO_SRC_UNSUPPORTED; }
    return AUDIO_SRC_OK;
}

size_t audio_source_read(audio_source_t *s, int16_t *out, size_t frames)
{
    if (s->format == AUDIO_FORMAT_WAV) {
        if (frames > s->wav_frames_left) frames = (size_t)s->wav_frames_left;
        size_t bytes = frames * s->channels * 2;
        size_t got = bytes ? s->io.read(s->io.ctx, out, bytes) : 0;
        size_t done = got / (s->channels * 2u);
        s->wav_frames_left -= done;
        if (got < bytes) s->error = true;   /* the header promised these bytes */
        return done;                        /* little-endian host and target */
    }
    if (!s->flac) return 0;
    flac_state_t *f = s->flac;
    size_t got = (size_t)drflac_read_pcm_frames_s16(f->decoder, frames, out);
    /* Short of the recorded length means a truncated or corrupt stream. */
    if (got < frames && s->total_frames && f->decoder->currentPCMFrame < s->total_frames) s->error = true;
    return got;
}

bool audio_source_rewind(audio_source_t *s)
{
    if (s->format == AUDIO_FORMAT_WAV) {
        if (!s->io.seek(s->io.ctx, s->wav.data_offset)) return false;
        s->wav_frames_left = s->total_frames;
        return true;
    }
    return s->flac && drflac_seek_to_pcm_frame(((flac_state_t *)s->flac)->decoder, 0);
}

void audio_source_close(audio_source_t *s)
{
    if (s->flac) {
        flac_state_t *f = s->flac;
        drflac_close(f->decoder);
        free(f);
        s->flac = NULL;
    }
}

const char *audio_src_text(audio_src_result_t r)
{
    switch (r) {
    case AUDIO_SRC_OK:          return "ok";
    case AUDIO_SRC_INVALID:     return "not a readable WAV/FLAC stream";
    case AUDIO_SRC_UNSUPPORTED: return "unsupported format (WAV: 16-bit PCM; both: mono/stereo, 8-48 kHz)";
    case AUDIO_SRC_NO_MEMORY:   return "out of memory";
    }
    return "unknown";
}

audio_src_result_t audio_probe(const char *name, FILE *f, uint64_t size)
{
    audio_io_t io = audio_io_file(f, size);
    audio_source_t s;
    audio_src_result_t r = audio_source_open(&s, audio_format_from_name(name), &io);
    if (r != AUDIO_SRC_OK) return r;
    /* Decode one block's worth to catch a damaged first frame (heap: on the
     * target this goes to PSRAM rather than a task stack). */
    size_t want = 4608;
    if (s.total_frames && s.total_frames < want) want = (size_t)s.total_frames;
    int16_t *probe = malloc(want * s.channels * sizeof *probe);
    bool ok = probe && audio_source_read(&s, probe, want) == want && !s.error;
    free(probe);
    audio_source_close(&s);
    if (!probe) return AUDIO_SRC_NO_MEMORY;
    return ok ? AUDIO_SRC_OK : AUDIO_SRC_INVALID;
}
