#include "wav_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char buf[512];
static size_t len;

static void put(const void *p, size_t n) { memcpy(buf + len, p, n); len += n; }
static void u16(unsigned v) { unsigned char b[2] = {v & 255, v >> 8 & 255}; put(b, 2); }
static void u32(unsigned long v) { unsigned char b[4] = {v & 255, v >> 8 & 255, v >> 16 & 255, v >> 24 & 255}; put(b, 4); }
static void chunk(const char *id, unsigned long size) { put(id, 4); u32(size); }
static void riff(void) { len = 0; chunk("RIFF", 0); put("WAVE", 4); }
static void fmt(unsigned format, unsigned ch, unsigned long rate, unsigned bits)
{
    chunk("fmt ", 16); u16(format); u16(ch); u32(rate); u32(rate * ch * bits / 8);
    u16(ch * bits / 8); u16(bits);
}

static wav_result_t parse(wav_info_t *info)
{
    FILE *f = fmemopen(buf, len, "rb");
    assert(f);
    wav_result_t r = wav_parse(f, len, info);
    fclose(f);
    return r;
}

int main(void)
{
    wav_info_t w;

    /* Plain PCM with a LIST chunk and an odd-sized chunk before data. */
    riff(); fmt(1, 2, 44100, 16);
    chunk("LIST", 3); put("abc", 3); put("\0", 1);   /* pad byte */
    chunk("data", 8); put("\1\2\3\4\5\6\7\10", 8);
    assert(parse(&w) == WAV_OK);
    assert(w.sample_rate == 44100 && w.channels == 2 && w.bits_per_sample == 16);
    assert(w.data_offset == len - 8 && w.data_bytes == 8);

    /* A data size past the end (streamed writer) is clamped to whole frames. */
    riff(); fmt(1, 2, 48000, 16); chunk("data", 0xffffffffUL); put("\1\2\3\4\5\6", 6);
    assert(parse(&w) == WAV_OK && w.data_bytes == 4);

    /* WAVE_FORMAT_EXTENSIBLE with the PCM subformat. */
    riff(); chunk("fmt ", 40); u16(0xfffe); u16(1); u32(16000); u32(32000); u16(2); u16(16);
    u16(22); u16(16); u32(4);
    put("\x01\x00\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71", 16);
    chunk("data", 2); put("\1\2", 2);
    assert(parse(&w) == WAV_OK && w.channels == 1 && w.sample_rate == 16000);
    buf[12 + 8 + 24] = 3;                          /* IEEE float subformat */
    assert(parse(&w) == WAV_UNSUPPORTED);

    /* Unsupported but well-formed. */
    riff(); fmt(1, 2, 44100, 24); chunk("data", 6); put("\0\0\0\0\0\0", 6);
    assert(parse(&w) == WAV_UNSUPPORTED);
    riff(); fmt(1, 3, 44100, 16); chunk("data", 6); put("\0\0\0\0\0\0", 6);
    assert(parse(&w) == WAV_UNSUPPORTED);
    riff(); fmt(1, 2, 96000, 16); chunk("data", 4); put("\0\0\0\0", 4);
    assert(parse(&w) == WAV_UNSUPPORTED);
    riff(); fmt(3, 2, 44100, 16); chunk("data", 4); put("\0\0\0\0", 4);
    assert(parse(&w) == WAV_UNSUPPORTED);

    /* Structural errors. */
    riff(); chunk("data", 4); put("\0\0\0\0", 4); fmt(1, 2, 44100, 16);
    assert(parse(&w) == WAV_BAD_CHUNKS);           /* data before fmt */
    riff(); fmt(1, 2, 44100, 16);
    assert(parse(&w) == WAV_BAD_CHUNKS);           /* no data */
    riff(); fmt(1, 2, 44100, 16); chunk("data", 4); put("\0\0", 2);
    assert(parse(&w) == WAV_BAD_CHUNKS);           /* less than one frame */
    riff(); fmt(1, 2, 44100, 16); len -= 4;
    assert(parse(&w) == WAV_BAD_CHUNKS);           /* truncated fmt */
    riff(); memcpy(buf + 8, "AVI ", 4);
    assert(parse(&w) == WAV_NOT_RIFF);
    len = 4;
    assert(parse(&w) == WAV_NOT_RIFF);
    puts("WAV header parsing, chunk skipping and format limits OK");
}
