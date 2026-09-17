#include <string.h>
#include <unity.h>
#include "sha256.h"
#include "utils.h"
#include "aes128.h"
#include "identity.h"

void setUp(void) {}
void tearDown(void) {}

// FIPS-197 Appendix B AES-128 test vector -- used as the AES key/oracle
// throughout. aes128_encrypt_block/aes128_decrypt_block are independently
// verified against this vector already (see lib/aes); here they're used
// as the ground truth to check that encrypt()/decrypt() in utils.c chunk,
// pad, and key the cipher correctly.
static const uint8_t key[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};
static const uint8_t block0_plain[16] = {
    0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
    0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34
};
static const uint8_t block0_cipher[16] = {
    0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
    0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32
};

void test_encrypt_single_block_matches_aes128(void) {
    uint8_t dest[16];

    uint32_t len = encrypt(key, dest, block0_plain, 16);

    TEST_ASSERT_EQUAL_UINT32(16, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_cipher, dest, 16);
}

void test_encrypt_multi_block(void) {
    uint8_t plain[32];
    memcpy(plain, block0_plain, 16);
    memcpy(plain + 16, block0_plain, 16); // same plaintext repeated -> ECB gives same cipher per block

    uint8_t dest[32];
    uint32_t len = encrypt(key, dest, plain, 32);

    TEST_ASSERT_EQUAL_UINT32(32, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_cipher, dest, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_cipher, dest + 16, 16);
}

void test_encrypt_zero_pads_partial_block(void) {
    // 20 bytes: one full block + 4 leftover bytes, expected to be
    // zero-padded to a full 16-byte block before encrypting.
    uint8_t plain[20];
    memcpy(plain, block0_plain, 16);
    memcpy(plain + 16, "boop", 4);

    uint8_t expected_last_block_plain[16] = {0};
    memcpy(expected_last_block_plain, "boop", 4);
    uint8_t expected_last_block_cipher[16];
    aes128_ctx aes;
    aes128_set_key(&aes, key);
    aes128_encrypt_block(&aes, expected_last_block_cipher, expected_last_block_plain);

    uint8_t dest[32];
    uint32_t len = encrypt(key, dest, plain, 20);

    TEST_ASSERT_EQUAL_UINT32(32, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_cipher, dest, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_last_block_cipher, dest + 16, 16);
}

void test_decrypt_single_block_matches_aes128(void) {
    uint8_t dest[16];

    uint32_t len = decrypt(key, dest, block0_cipher, 16);

    TEST_ASSERT_EQUAL_UINT32(16, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_plain, dest, 16);
}

void test_decrypt_multi_block(void) {
    uint8_t cipher[32];
    memcpy(cipher, block0_cipher, 16);
    memcpy(cipher + 16, block0_cipher, 16);

    uint8_t dest[32];
    uint32_t len = decrypt(key, dest, cipher, 32);

    TEST_ASSERT_EQUAL_UINT32(32, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_plain, dest, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(block0_plain, dest + 16, 16);
}

void test_encrypt_decrypt_round_trip(void) {
    const char *message = "the quick brown fox";
    uint32_t msg_len = (uint32_t)strlen(message);

    uint8_t cipher[32];
    uint32_t cipher_len = encrypt(key, cipher, (const uint8_t*)message, msg_len);
    TEST_ASSERT_EQUAL_UINT32(32, cipher_len); // 20 bytes -> padded to 2 blocks

    uint8_t plain[32] = {0};
    uint32_t plain_len = decrypt(key, plain, cipher, cipher_len);
    TEST_ASSERT_EQUAL_UINT32(cipher_len, plain_len);

    // original message bytes must round-trip exactly...
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t*)message, plain, msg_len);
    // ...and the zero-padding must round-trip back to zero.
    uint8_t zeros[32] = {0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, plain + msg_len, cipher_len - msg_len);
}

// HMAC-SHA256 test vectors -- generated with Python's hmac/hashlib and
// cross-checked against `openssl dgst -sha256 -mac HMAC` independently
// (both agreed byte-for-byte) before being hardcoded here.
static const uint8_t hmac_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const char *hmac_message = "synthpass test message";
// HMAC-SHA256(hmac_key, hmac_message), full 32-byte output.
static const uint8_t hmac_expected[32] = {
    0xe5, 0xb6, 0x14, 0xf8, 0xc2, 0xbc, 0xb2, 0xac,
    0x78, 0x2d, 0x05, 0x94, 0x57, 0x0c, 0x80, 0xa4,
    0xa6, 0x5b, 0x64, 0x5a, 0x7e, 0x7b, 0xc6, 0x95,
    0x4c, 0x57, 0xd6, 0xc5, 0x71, 0x5d, 0x2a, 0xdb
};
// HMAC-SHA256(hmac_key, "the quick brown " + "fox") -- same message as
// above's single-shot append, but fed to hmac_sha256 in two separate
// sha256_append() calls, to check the streaming/inner-hash state carries
// across multiple appends correctly.
static const uint8_t hmac_multi_expected[32] = {
    0xd4, 0x2d, 0x6a, 0xfb, 0x45, 0xf6, 0xf5, 0xfe,
    0x9d, 0x6a, 0xe6, 0xa2, 0xf7, 0xc3, 0x31, 0xe5,
    0x0c, 0xf1, 0x45, 0x40, 0x99, 0x5d, 0xa0, 0x7a,
    0x65, 0x9d, 0x02, 0xd1, 0x07, 0x76, 0x25, 0xa2
};

void test_hmac_sha256_full_output(void) {
    hmac_sha256_ctx ctx;
    hmac_sha256_reset(&ctx, hmac_key);
    sha256_append(&ctx.inner, hmac_message, strlen(hmac_message));

    uint8_t out[32];
    hmac_sha256_finalize(&ctx, out, 32);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(hmac_expected, out, 32);
}

void test_hmac_sha256_truncated_output(void) {
    // CIPHER_MAC_SIZE (2 bytes) is what encryptThenMAC/MACThenDecrypt actually use.
    hmac_sha256_ctx ctx;
    hmac_sha256_reset(&ctx, hmac_key);
    sha256_append(&ctx.inner, hmac_message, strlen(hmac_message));

    uint8_t out[2];
    hmac_sha256_finalize(&ctx, out, 2);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(hmac_expected, out, 2);
}

void test_hmac_sha256_multiple_appends(void) {
    hmac_sha256_ctx ctx;
    hmac_sha256_reset(&ctx, hmac_key);
    sha256_append(&ctx.inner, "the quick brown ", 16);
    sha256_append(&ctx.inner, "fox", 3);

    uint8_t out[32];
    hmac_sha256_finalize(&ctx, out, 32);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(hmac_multi_expected, out, 32);
}

void test_hmac_sha256_different_key_differs(void) {
    uint8_t other_key[32];
    memcpy(other_key, hmac_key, 32);
    other_key[0] ^= 0xFF; // flip a byte

    hmac_sha256_ctx ctx;
    hmac_sha256_reset(&ctx, other_key);
    sha256_append(&ctx.inner, hmac_message, strlen(hmac_message));

    uint8_t out[32];
    hmac_sha256_finalize(&ctx, out, 32);

    TEST_ASSERT_NOT_EQUAL(0, memcmp(hmac_expected, out, 32));
}

// encryptThenMAC/MACThenDecrypt test vectors: 32-byte key = 0x00..0x1f (same
// key convention as the hmac_sha256 tests above), message "synthpass test
// message". Independently computed with Python's `cryptography` (AES-128-ECB,
// zero-padded) + `hmac`/`hashlib` (HMAC-SHA256 over the ciphertext, truncated
// to CIPHER_MAC_SIZE) and confirmed to match a scratch C harness calling the
// real encryptThenMAC() byte-for-byte before being hardcoded here.
static const uint8_t etm_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const char *etm_message = "synthpass test message"; // 23 bytes -> 2 AES blocks
static const uint8_t etm_expected_mac[CIPHER_MAC_SIZE] = {0xa2, 0x30};
static const uint8_t etm_expected_ciphertext[32] = {
    0xea, 0xd4, 0x6b, 0x71, 0x09, 0x4d, 0xe1, 0x32,
    0x0f, 0x30, 0x95, 0xe3, 0x0b, 0x01, 0xcf, 0x16,
    0x97, 0xf8, 0xba, 0x9b, 0x77, 0x32, 0xed, 0xfc,
    0x5a, 0x77, 0x56, 0xee, 0xe0, 0x1e, 0x00, 0x77
};

void test_encryptThenMAC_matches_reference(void) {
    uint32_t msg_len = (uint32_t)strlen(etm_message);
    uint8_t dest[64];

    uint32_t out_len = encryptThenMAC(etm_key, dest, (const uint8_t*)etm_message, msg_len);

    TEST_ASSERT_EQUAL_UINT32(CIPHER_MAC_SIZE + 32, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(etm_expected_mac, dest, CIPHER_MAC_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(etm_expected_ciphertext, dest + CIPHER_MAC_SIZE, 32);
}

void test_MACThenDecrypt_matches_reference(void) {
    uint8_t framed[CIPHER_MAC_SIZE + 32];
    memcpy(framed, etm_expected_mac, CIPHER_MAC_SIZE);
    memcpy(framed + CIPHER_MAC_SIZE, etm_expected_ciphertext, 32);

    uint8_t plain[32] = {0};
    uint32_t plain_len = MACThenDecrypt(etm_key, plain, framed, CIPHER_MAC_SIZE + 32);

    TEST_ASSERT_EQUAL_UINT32(32, plain_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t*)etm_message, plain, strlen(etm_message));
}

void test_encryptThenMAC_MACThenDecrypt_round_trip(void) {
    uint32_t msg_len = (uint32_t)strlen(etm_message);
    uint8_t framed[64];

    uint32_t framed_len = encryptThenMAC(etm_key, framed, (const uint8_t*)etm_message, msg_len);

    uint8_t plain[64] = {0};
    uint32_t plain_len = MACThenDecrypt(etm_key, plain, framed, framed_len);

    TEST_ASSERT_EQUAL_UINT32(32, plain_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t*)etm_message, plain, msg_len);
}

void test_MACThenDecrypt_rejects_tampered_ciphertext(void) {
    uint8_t framed[CIPHER_MAC_SIZE + 32];
    memcpy(framed, etm_expected_mac, CIPHER_MAC_SIZE);
    memcpy(framed + CIPHER_MAC_SIZE, etm_expected_ciphertext, 32);
    framed[CIPHER_MAC_SIZE] ^= 0xFF; // corrupt the first ciphertext byte

    uint8_t plain[32] = {0};
    uint32_t plain_len = MACThenDecrypt(etm_key, plain, framed, CIPHER_MAC_SIZE + 32);

    TEST_ASSERT_EQUAL_UINT32(0, plain_len);
}

void test_MACThenDecrypt_rejects_wrong_mac(void) {
    uint8_t framed[CIPHER_MAC_SIZE + 32];
    memcpy(framed, etm_expected_mac, CIPHER_MAC_SIZE);
    framed[0] ^= 0xFF; // corrupt the MAC itself, ciphertext untouched
    memcpy(framed + CIPHER_MAC_SIZE, etm_expected_ciphertext, 32);

    uint8_t plain[32] = {0};
    uint32_t plain_len = MACThenDecrypt(etm_key, plain, framed, CIPHER_MAC_SIZE + 32);

    TEST_ASSERT_EQUAL_UINT32(0, plain_len);
}

// First 16 bytes of sha256(hashtag), per the protocol doc's "hashtag channel
// key = first 16 bytes of sha256(name)" rule. Independently computed with
// Python's hashlib.
static const uint8_t channel_key_test[16] = {
    0x9c, 0xd8, 0xfc, 0xf2, 0x2a, 0x47, 0x33, 0x3b,
    0x59, 0x1d, 0x96, 0xa2, 0xb8, 0x48, 0xb7, 0x3f
};
static const uint8_t channel_key_general[16] = {
    0x4c, 0x49, 0xf3, 0xf2, 0x46, 0x29, 0xf5, 0xee,
    0x4a, 0xd5, 0xb3, 0x96, 0x5d, 0xb4, 0x79, 0x85
};

void test_derive_channel_key_matches_sha256(void) {
    uint8_t out[16];
    derive_channel_key((uint8_t*)"#test", 5, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(channel_key_test, out, 16);
}

void test_derive_channel_key_different_name_differs(void) {
    uint8_t out[16];
    derive_channel_key((uint8_t*)"#general", 8, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(channel_key_general, out, 16);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(channel_key_test, out, 16));
}

void test_derive_channel_key_deterministic(void) {
    uint8_t out1[16], out2[16];
    derive_channel_key((uint8_t*)"#test", 5, out1);
    derive_channel_key((uint8_t*)"#test", 5, out2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(out1, out2, 16);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_encrypt_single_block_matches_aes128);
    RUN_TEST(test_encrypt_multi_block);
    RUN_TEST(test_encrypt_zero_pads_partial_block);
    RUN_TEST(test_decrypt_single_block_matches_aes128);
    RUN_TEST(test_decrypt_multi_block);
    RUN_TEST(test_encrypt_decrypt_round_trip);
    RUN_TEST(test_hmac_sha256_full_output);
    RUN_TEST(test_hmac_sha256_truncated_output);
    RUN_TEST(test_hmac_sha256_multiple_appends);
    RUN_TEST(test_hmac_sha256_different_key_differs);
    RUN_TEST(test_encryptThenMAC_matches_reference);
    RUN_TEST(test_MACThenDecrypt_matches_reference);
    RUN_TEST(test_encryptThenMAC_MACThenDecrypt_round_trip);
    RUN_TEST(test_MACThenDecrypt_rejects_tampered_ciphertext);
    RUN_TEST(test_MACThenDecrypt_rejects_wrong_mac);
    RUN_TEST(test_derive_channel_key_matches_sha256);
    RUN_TEST(test_derive_channel_key_different_name_differs);
    RUN_TEST(test_derive_channel_key_deterministic);
    return UNITY_END();
}
