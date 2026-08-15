#pragma once
#include <stdint.h>

#define MAX_HASH_SIZE        8
#define PUB_KEY_SIZE        32
#define PRV_KEY_SIZE        64
#define SEED_SIZE           32
#define SIGNATURE_SIZE      64
#define MAX_ADVERT_DATA_SIZE  32
#define CIPHER_KEY_SIZE     16
#define CIPHER_BLOCK_SIZE   16

#define CIPHER_MAC_SIZE      2
#define PATH_HASH_SIZE       1


typedef struct {
    uint8_t pub_key[PUB_KEY_SIZE];
} PM_Identity;

uint8_t id_verify(PM_Identity *id, uint8_t *sig, const uint8_t *message, int msg_len);

// uint8_t identity_matches(PM_Identity *id, PM_Identity *other);// { return memcmp(id->pub_key, other->pub_key, PUB_KEY_SIZE) == 0; }
// uint8_t identity_matches_pubkey(PM_Identity *id, uint8_t *other_pubkey);// { return memcmp(id->pub_key, other_pubkey, PUB_KEY_SIZE) == 0; }

typedef struct {
    uint8_t pub_key[PUB_KEY_SIZE];
    uint8_t prv_key[PRV_KEY_SIZE];
} PM_LocalIdentity;

void local_id_sign(PM_LocalIdentity *id, uint8_t *sig, uint8_t *message, uint16_t msg_len);

void local_id_calc_shared_secrt(PM_LocalIdentity *id, uint8_t *secret, PM_Identity *other);

uint8_t local_id_validate_private_key(uint8_t prv[64]);
