#include "audio_source.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Fixtures were made with ffmpeg (440 Hz stereo tone, 0.25 s) and the flac
 * CLI (--best --no-padding); tone24 is the same PCM widened to 24 bits. */
#define DATA DATA_DIR "/"

static FILE *open_file(const char *path, uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    struct stat st;
    assert(f && !fstat(fileno(f), &st));
    *size = (uint64_t)st.st_size;
    return f;
}

/* Decode a whole file in odd-sized chunks; returns frames, fills *pcm. */
static size_t decode_all(const char *path, int16_t **pcm, audio_source_t *info)
{
    uint64_t size;
    FILE *f = open_file(path, &size);
    audio_io_t io = audio_io_file(f, size);
    audio_source_t s;
    assert(audio_source_open(&s, audio_format_from_name(path), &io) == AUDIO_SRC_OK);
    size_t cap = 1 << 16, total = 0, got;
    *pcm = malloc(cap * s.channels * sizeof **pcm);
    while ((got = audio_source_read(&s, *pcm + total * s.channels, 777)) > 0) total += got;
    assert(!s.error);
    *info = s;
    audio_source_close(&s);
    fclose(f);
    return total;
}

static audio_src_result_t probe_bytes(const char *name, const unsigned char *data, size_t n)
{
    FILE *f = fmemopen((void *)data, n, "rb");
    audio_src_result_t r = audio_probe(name, f, n);
    fclose(f);
    return r;
}

static unsigned char *slurp(const char *path, size_t *n)
{
    uint64_t size;
    FILE *f = open_file(path, &size);
    unsigned char *buf = malloc(size);
    assert(fread(buf, 1, size, f) == size);
    fclose(f);
    *n = (size_t)size;
    return buf;
}

int main(void)
{
    assert(audio_format_from_name("sounds/a.wav") == AUDIO_FORMAT_WAV);
    assert(audio_format_from_name("sounds/a.flac") == AUDIO_FORMAT_FLAC);
    assert(audio_format_from_name("sounds/a.FLAC") == AUDIO_FORMAT_UNKNOWN);
    assert(audio_format_from_name("sounds/a.mp3") == AUDIO_FORMAT_UNKNOWN);

    /* FLAC 16- and 24-bit decode bit-exactly to the reference WAV PCM. */
    int16_t *wav, *flac16, *flac24;
    audio_source_t wi, fi16, fi24;
    size_t nw = decode_all(DATA "tone16.wav", &wav, &wi);
    size_t n16 = decode_all(DATA "tone16.flac", &flac16, &fi16);
    size_t n24 = decode_all(DATA "tone24.flac", &flac24, &fi24);
    assert(nw == 11025 && wi.sample_rate == 44100 && wi.channels == 2);
    assert(n16 == nw && fi16.total_frames == nw && fi16.source_bits == 16);
    assert(n24 == nw && fi24.source_bits == 24);
    assert(!memcmp(wav, flac16, nw * 4) && !memcmp(wav, flac24, nw * 4));

    /* Rewind restarts at frame 0 for seamless loops. */
    uint64_t size;
    FILE *f = open_file(DATA "tone16.flac", &size);
    audio_io_t io = audio_io_file(f, size);
    audio_source_t s;
    int16_t buf[3000 * 2];
    assert(audio_source_open(&s, AUDIO_FORMAT_FLAC, &io) == AUDIO_SRC_OK);
    assert(audio_source_read(&s, buf, 3000) == 3000);
    assert(audio_source_read(&s, buf, 3000) == 3000);
    assert(audio_source_rewind(&s));
    assert(audio_source_read(&s, buf, 3000) == 3000 && !memcmp(buf, wav, 3000 * 4));
    audio_source_close(&s);
    fclose(f);
    f = open_file(DATA "tone16.wav", &size);
    io = audio_io_file(f, size);
    assert(audio_source_open(&s, AUDIO_FORMAT_WAV, &io) == AUDIO_SRC_OK);
    while (audio_source_read(&s, buf, 3000)) {}
    assert(audio_source_rewind(&s) && audio_source_read(&s, buf, 3000) == 3000 && !memcmp(buf, wav, 3000 * 4));
    audio_source_close(&s);
    fclose(f);

    /* Upload probe: limits and damage. */
    f = open_file(DATA "mono8k.flac", &size);
    assert(audio_probe("x.flac", f, size) == AUDIO_SRC_OK);
    fclose(f);
    f = open_file(DATA "hires96k.flac", &size);
    assert(audio_probe("x.flac", f, size) == AUDIO_SRC_UNSUPPORTED);
    fclose(f);
    size_t n;
    unsigned char *bytes = slurp(DATA "tone16.flac", &n);
    assert(probe_bytes("x.flac", bytes, n) == AUDIO_SRC_OK);
    assert(probe_bytes("x.wav", bytes, n) == AUDIO_SRC_INVALID);    /* extension decides */
    assert(probe_bytes("x.flac", bytes, 60) == AUDIO_SRC_INVALID);  /* metadata only */
    bytes[0] = 'X';                                                  /* not fLaC */
    assert(probe_bytes("x.flac", bytes, n) == AUDIO_SRC_INVALID);
    bytes[0] = 'f';
    /* A truncated stream decodes its first frame but reports an error later. */
    FILE *t = fmemopen(bytes, n / 2, "rb");
    io = audio_io_file(t, n / 2);
    assert(audio_source_open(&s, AUDIO_FORMAT_FLAC, &io) == AUDIO_SRC_OK);
    size_t frames = 0, got;
    while ((got = audio_source_read(&s, buf, 1000)) > 0) frames += got;
    assert(frames < nw && s.error);
    audio_source_close(&s);
    fclose(t);
    free(bytes);
    free(wav); free(flac16); free(flac24);
    puts("WAV/FLAC decoding is bit-exact; rewind, limits and damage detection OK");
}
