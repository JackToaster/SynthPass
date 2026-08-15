#include <string.h>
#include <unity.h>
#include "packet.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

void test_pack_header_dnr(void) {
    PM_Header h = { .do_not_retransmit = 1 };
    TEST_ASSERT_EQUAL_UINT8(0xFF, pack_header(h));
}

void test_header_roundtrip(void) {
    PM_Header h = {
        .route_type = ROUTE_TYPE_FLOOD,
        .payload_type = PAYLOAD_TYPE_ACK,
        .version = 0
    };

    uint8_t packed = pack_header(h);
    PM_Header h2 = unpack_header(packed);

    TEST_ASSERT_EQUAL_UINT8(h.route_type, h2.route_type);
    TEST_ASSERT_EQUAL_UINT8(h.payload_type, h2.payload_type);
    TEST_ASSERT_EQUAL_UINT8(h.version, h2.version);
}

void test_calculate_hash_matches_meshcore(void) {
    PM_Packet packet = {0};
    packet.header.payload_type = PAYLOAD_TYPE_TXT_MSG;
    const char *msg = "hello mesh";
    packet.payload.len = (uint8_t)strlen(msg);
    memcpy(packet.payload.data, msg, packet.payload.len);

    // SHA256(02 68656c6c6f206d657368) = 8ba96957bf656833b2968c6e00a7ffa..
    static const uint8_t expected[MAX_HASH_SIZE] = {
        0x8b, 0xa9, 0x69, 0x57, 0xbf, 0x65, 0x68, 0x33
    };

    // bigger (check for overflow)
    uint8_t hash[32];
    memset(hash, 0, sizeof(hash));

    packet_calculate_hash(&packet, hash);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, hash, MAX_HASH_SIZE);
    TEST_ASSERT_EQUAL_UINT8(0, hash[MAX_HASH_SIZE]); // not touch after hash
}

void test_calculate_hash_trace_packet(void) {
    PM_Packet packet = {0};
    packet.header.payload_type = PAYLOAD_TYPE_TRACE;
    packet.path_len.hop_hash_size = 1;
    packet.path_len.hops = 3;
    const char *payload = "hop test";
    packet.payload.len = (uint8_t)strlen(payload);
    memcpy(packet.payload.data, payload, packet.payload.len);

    //   SHA256(09 03 00 686f702074657374) = b561f1c043b81a5076fe37fc42457ac...
    static const uint8_t expected[MAX_HASH_SIZE] = {
        0xb5, 0x61, 0xf1, 0xc0, 0x43, 0xb8, 0x1a, 0x50
    };

    uint8_t hash[32];
    memset(hash, 0, sizeof(hash));

    packet_calculate_hash(&packet, hash);
   
    // printf("hash:\n");
    // for (int i = 0; i < sizeof(hash); i++) printf(" %02x", hash[i]);
    // printf("\n");
    
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, hash, MAX_HASH_SIZE);
    TEST_ASSERT_EQUAL_UINT8(0, hash[MAX_HASH_SIZE]); // not touch after hash
}

void test_packet_round_trip(void) {
    uint8_t hops[1] = {'a'};

    const char *payload = "hop test";
    PM_Packet p = {
        .header={
            .payload_type=PAYLOAD_TYPE_TXT_MSG,
            .route_type=ROUTE_TYPE_FLOOD,
        },
        .path_len={
            .hop_hash_size=1,
            .hops=1,
        },
        .payload={
            .len=strlen(payload)
        }
    };

    memcpy(p.path, hops, sizeof(hops));
    memcpy(p.payload.data, payload, strlen(payload));

    uint8_t buf[MAX_TRANS_UNIT];
    memset(buf, 0, MAX_TRANS_UNIT);

    uint8_t len = pack_packet(&p, buf);

    TEST_ASSERT_EQUAL_UINT8(0, buf[len]);
    TEST_ASSERT_EQUAL_UINT8(11, len);

    PM_Packet p2;
    memset(&p2, 0, sizeof(p2));
    uint8_t *b = (uint8_t*)(void*)&p2;

    // printf("p2 init:\n");
    // for (int i = 0; i < sizeof(p2); i++) printf(" %02x", b[i]);
    // printf("\n");

    // printf("p2 set 0:\n");
    // for (int i = 0; i < sizeof(p2); i++) printf(" %02x", b[i]);
    // printf("\n");
    // printf("\n");

    // printf("buf:\n");
    // for (int i = 0; i < sizeof(buf); i++) printf(" %02x", buf[i]);
    // printf("\n");
    // printf("\n");

    unpack_packet(buf, len, &p2);

    uint8_t *a = (uint8_t*)(void*)&p;
    // uint8_t *b = (uint8_t*)(void*)&p2;

    // printf(  "expected:");
    // for (int i = 0; i < sizeof(p); i++) printf(" %02x", a[i]);
    // printf("\ngot:     ");
    // for (int i = 0; i < sizeof(p2); i++) printf(" %02x", b[i]);
    // printf("\n");

    // we check the whole thing to see if anything wrote beyond buffers.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, sizeof(PM_Packet));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack_header_dnr);
    RUN_TEST(test_header_roundtrip);
    RUN_TEST(test_calculate_hash_matches_meshcore);
    RUN_TEST(test_calculate_hash_trace_packet);
    RUN_TEST(test_packet_round_trip);
    return UNITY_END();
}