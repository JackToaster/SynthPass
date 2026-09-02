#include <string.h>
#include "utils.h"
#include "aes128.h"
#include "identity.h"
#include "sha256.h"


// caller needs to check input length, len is unbounded/unchecked here
void derive_channel_key(uint8_t *hashtag_name, uint32_t len, uint8_t out_key16[16]) {
  struct sha256 sha;
  sha256_init(&sha);
  sha256_append(&sha, hashtag_name, len);
  sha256_finalize_bytes(&sha, out_key16, 16);
}

uint32_t decrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, uint32_t src_len) {
  aes128_ctx aes;

  uint8_t *dp = dest;
  const uint8_t *sp = src;
  
  aes128_set_key(&aes, shared_secret);
  
  while(sp - src < src_len) {
    aes128_decrypt_block(&aes, dp, sp);
    dp += 16; sp += 16;
  }
  return sp - src;
}



uint32_t encrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, uint32_t src_len){
  aes128_ctx aes;
  uint8_t* dp = dest;

  aes128_set_key(&aes, shared_secret);
  while (src_len >= 16) {
    aes128_encrypt_block(&aes, dp, src);
    dp += 16; src += 16; src_len -= 16;
  }
  if (src_len > 0) {  // remaining partial block
    uint8_t tmp[16];
    memset(tmp, 0, 16);
    memcpy(tmp, src, src_len);
    aes128_encrypt_block(&aes, dp, tmp);
    dp += 16;
  }
  return dp - dest;  // will always be multiple of 16
}

// sha256 HMAC helper functions
void hmac_sha256_reset(hmac_sha256_ctx *ctx, const uint8_t key32[32]) {
  uint8_t key_block[64] = {0};
  memcpy(key_block, key32, 32);

  uint8_t k_ipad[64] = {0};

  for(uint8_t i = 0; i < 64; ++i){
    k_ipad[i] = key_block[i] ^ 0x36;
    ctx->k_opad[i] = key_block[i] ^ 0x5c;
  }

  sha256_init(&ctx->inner);
  sha256_append(&ctx->inner, k_ipad, 64);
}

void hmac_sha256_finalize(hmac_sha256_ctx *ctx, const uint8_t *out, uint32_t out_len) {
  uint8_t inner_hash[32];
  sha256_finalize_bytes(&ctx->inner, inner_hash, 32);
  struct sha256 outer;
  sha256_init(&outer);
  sha256_append(&outer, ctx->k_opad, 64);
  sha256_append(&outer, inner_hash, 32);
  sha256_finalize_bytes(&outer, out, out_len);
}


uint32_t encryptThenMAC(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len) {
  uint32_t enc_len = encrypt(shared_secret, dest + CIPHER_MAC_SIZE, src, src_len);

  hmac_sha256_ctx hmac;
  hmac_sha256_reset(&hmac, shared_secret);
  sha256_append(&hmac.inner, dest + CIPHER_MAC_SIZE, enc_len);
  hmac_sha256_finalize(&hmac, dest, CIPHER_MAC_SIZE);
  return CIPHER_MAC_SIZE + enc_len;
}

uint32_t MACThenDecrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len) {
  if(src_len < CIPHER_MAC_SIZE) return 0; // invalid src bytes
  uint8_t hmac_bytes[CIPHER_MAC_SIZE];

  hmac_sha256_ctx hmac;
  hmac_sha256_reset(&hmac, shared_secret);
  sha256_append(&hmac.inner, src + CIPHER_MAC_SIZE, src_len - CIPHER_MAC_SIZE);
  hmac_sha256_finalize(&hmac, hmac_bytes, CIPHER_MAC_SIZE);

  // TODO this should technically be a constant time comparison to prevent
  // timing attacks - doesn't seem very plausible regardless, but maybe worth
  // doing anyway
  if(memcmp(hmac_bytes, src, CIPHER_MAC_SIZE) == 0) {
    return decrypt(shared_secret, dest, src + CIPHER_MAC_SIZE, src_len - CIPHER_MAC_SIZE);
  } else {
    return 0; // invalid HMAC
  }
}
