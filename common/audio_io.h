#ifndef RLCD_AUDIO_IO_H
#define RLCD_AUDIO_IO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Byte-stream callbacks for audio parsing and decoding. The firmware wraps
 * them around the storage mutex; host tests and upload validation use a
 * plain FILE. seek is absolute; callers keep offsets within size. */
typedef struct {
    size_t (*read)(void *ctx, void *buf, size_t n);
    bool (*seek)(void *ctx, uint64_t offset);
    void *ctx;
    uint64_t size;
} audio_io_t;

/* FILE-backed io; f must stay open while the io is used. */
audio_io_t audio_io_file(FILE *f, uint64_t size);
#endif
