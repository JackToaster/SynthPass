#include "identity.h"
#include "ed_25519.h"
#include <string.h>

uint8_t id_verify(PM_Identity *id, uint8_t *sig, const uint8_t *message, int msg_len) {
    return ed25519_verify(sig, message, msg_len, id->pub_key);
}


void local_id_sign(PM_LocalIdentity *id, uint8_t *sig, uint8_t *message, uint16_t msg_len) {
    ed25519_sign(sig, message, msg_len, id->pub_key, id->prv_key);
}

void local_id_calc_shared_secrt(PM_LocalIdentity *id, uint8_t *secret, PM_Identity *other) {
    ed25519_key_exchange(secret, other->pub_key, id->prv_key);
}

uint8_t local_id_validate_private_key(uint8_t prv[64]) {
    uint8_t pub[32];
    ed25519_derive_pub(pub, prv);

    // disallow 00 or FF prefixed public keys
    if (pub[0] == 0x00 || pub[0] == 0xFF) return 0;

    // known good test client keypair
    const uint8_t test_client_prv[64] = {
      0x70, 0x65, 0xe1, 0x8f, 0xd9, 0xfa, 0xbb, 0x70,
      0xc1, 0xed, 0x90, 0xdc, 0xa1, 0x99, 0x07, 0xde,
      0x69, 0x8c, 0x88, 0xb7, 0x09, 0xea, 0x14, 0x6e,
      0xaf, 0xd9, 0x3d, 0x9b, 0x83, 0x0c, 0x7b, 0x60,
      0xc4, 0x68, 0x11, 0x93, 0xc7, 0x9b, 0xbc, 0x39,
      0x94, 0x5b, 0xa8, 0x06, 0x41, 0x04, 0xbb, 0x61,
      0x8f, 0x8f, 0xd7, 0xa8, 0x4a, 0x0a, 0xf6, 0xf5,
      0x70, 0x33, 0xd6, 0xe8, 0xdd, 0xcd, 0x64, 0x71
    };
    const uint8_t test_client_pub[32] = {
      0x1e, 0xc7, 0x71, 0x75, 0xb0, 0x91, 0x8e, 0xd2,
      0x06, 0xf9, 0xae, 0x04, 0xec, 0x13, 0x6d, 0x6d,
      0x5d, 0x43, 0x15, 0xbb, 0x26, 0x30, 0x54, 0x27,
      0xf6, 0x45, 0xb4, 0x92, 0xe9, 0x35, 0x0c, 0x10
    };

    uint8_t ss1[32], ss2[32];

    // shared secret we calculte from test client pubkey and given private key
    ed25519_key_exchange(ss1, test_client_pub, prv);

    // shared secret they calculate from our derived public key and test client private key
    ed25519_key_exchange(ss2, pub, test_client_prv);

    // check that both shared secrets match
    if (memcmp(ss1, ss2, 32) != 0) return 0;

    // reject all-zero shared secret
    for (int i = 0; i < 32; i++) {
        if (ss1[i] != 0) return 1;
    }

    return 0;
}
