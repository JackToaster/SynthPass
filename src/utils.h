#pragma once
#include <stdint.h>
#include "sha256.h"

#define PUBLIC_CHANNEL_KEY 8b3387e9c5cdea6ac9e5edbaa115cd72

void derive_channel_key(uint8_t *hashtag_name, uint32_t len, uint8_t out_key16[16]);


uint32_t decrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, uint32_t src_len);
uint32_t encrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, uint32_t src_len);


// SAH256 HMAC helper functions

typedef struct {
  struct sha256 inner;
  uint8_t k_opad[64];
} hmac_sha256_ctx;

void hmac_sha256_reset(hmac_sha256_ctx *ctx, const uint8_t key32[32]);
void hmac_sha256_finalize(hmac_sha256_ctx *ctx, const uint8_t *out, uint32_t out_len);

uint32_t encryptThenMAC(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len);
uint32_t MACThenDecrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len);
