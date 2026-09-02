#ifndef AES128_H
#define AES128_H

#include <stdint.h>

// AES-128 block cipher, ECB single-block encrypt/decrypt.
// Ported to plain C from rweather/Crypto's AESCommon/AES128 classes
// (the same block cipher MeshCore uses via <AES.h>). See LICENSE.

#define AES128_KEY_SIZE   16
#define AES128_BLOCK_SIZE 16
#define AES128_ROUNDS     10

typedef struct {
    uint8_t schedule[(AES128_ROUNDS + 1) * AES128_BLOCK_SIZE]; // 176 bytes
} aes128_ctx;

void aes128_set_key(aes128_ctx *ctx, const uint8_t key[AES128_KEY_SIZE]);

void aes128_encrypt_block(const aes128_ctx *ctx, uint8_t output[AES128_BLOCK_SIZE], const uint8_t input[AES128_BLOCK_SIZE]);

void aes128_decrypt_block(const aes128_ctx *ctx, uint8_t output[AES128_BLOCK_SIZE], const uint8_t input[AES128_BLOCK_SIZE]);

// Zero the key schedule (best-effort against compiler optimization, mirrors rweather/Crypto's clean()).
void aes128_clear(aes128_ctx *ctx);

#endif
