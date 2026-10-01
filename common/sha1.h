#ifndef RLCD_SHA1_H
#define RLCD_SHA1_H
#include <stddef.h>
#include <stdint.h>

/* Minimal SHA-1 for verifying the IERS leap-seconds.list integrity hash.
 * It is a file-damage check, not an authenticity guarantee. */
typedef struct {
    uint32_t h[5];
    uint64_t length;
    uint8_t block[64];
    size_t used;
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *c);
void sha1_update(sha1_ctx_t *c, const void *data, size_t n);
void sha1_final(sha1_ctx_t *c, uint32_t digest[5]);
#endif
