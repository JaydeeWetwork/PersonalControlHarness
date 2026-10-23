/* sha256.h - small portable SHA-256 (FIPS 180-4) for the Black Box prototype. */
#ifndef BB_SHA256_H
#define BB_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } bb_sha256_ctx;
void bb_sha256_init(bb_sha256_ctx *c);
void bb_sha256_update(bb_sha256_ctx *c, const void *data, size_t len);
void bb_sha256_final(bb_sha256_ctx *c, uint8_t out[32]);
void bb_sha256(const void *data, size_t len, uint8_t out[32]);
/* HMAC-SHA256. Used ONLY as a stand-in for checkpoint signatures in the prototype. */
void bb_hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
#endif
