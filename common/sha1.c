#include "sha1.h"

static uint32_t rol(uint32_t x, unsigned n) { return x << n | x >> (32 - n); }

static void compress(sha1_ctx_t *c)
{
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t)c->block[4 * i] << 24 | (uint32_t)c->block[4 * i + 1] << 16 |
               (uint32_t)c->block[4 * i + 2] << 8 | c->block[4 * i + 3];
    for (unsigned i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (unsigned i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else             { f = b ^ cc ^ d;                   k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(sha1_ctx_t *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
    c->length = 0; c->used = 0;
}

void sha1_update(sha1_ctx_t *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->length += n;
    while (n--) {
        c->block[c->used++] = *p++;
        if (c->used == 64) { compress(c); c->used = 0; }
    }
}

void sha1_final(sha1_ctx_t *c, uint32_t digest[5])
{
    uint64_t bits = c->length * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56) sha1_update(c, &pad, 1);
    for (int i = 7; i >= 0; --i) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha1_update(c, &b, 1); }
    for (unsigned i = 0; i < 5; ++i) digest[i] = c->h[i];
}
