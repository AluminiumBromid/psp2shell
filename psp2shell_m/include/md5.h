#ifndef P2S_MD5_H
#define P2S_MD5_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[4];
    uint64_t bytes;
    uint8_t block[64];
} md5_ctx;

void md5_init(md5_ctx *ctx);

void md5_update(md5_ctx *ctx, const uint8_t *data, size_t len);

void md5_final(md5_ctx *ctx, uint8_t digest[16]);

#endif // P2S_MD5_H
