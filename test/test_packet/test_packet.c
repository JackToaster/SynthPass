#include <string.h>
#include <stdlib.h>
#include <unity.h>
#include "packet.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

// Unpack a frame from a heap buffer of exactly `len` bytes, so any read past the
// end of the frame is caught when running under ASan (env:native_asan).
static uint8_t unpack_exact(const uint8_t *frame, uint8_t len, PM_Packet *into) {
    uint8_t *buf = malloc(len ? len : 1);
    if (len) memcpy(buf, frame, len); // memcpy from NULL is UB even for 0 bytes
    uint8_t err = unpack_packet(buf, len, into);
    free(buf);
    return err;
}

// header byte for a TXT_MSG with the given route type
static uint8_t txt_header(uint8_t route_type) {
    PM_Header h = { .route_type = route_type, .payload_type = PAYLOAD_TYPE_TXT_MSG };
    return pack_header(h);
}

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

    uint8_t errcode = unpack_packet(buf, len, &p2);

    TEST_ASSERT_EQUAL_UINT8(0, errcode);

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

// ---- too short ----

void test_unpack_rejects_empty(void) {
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(NULL, 0, &p));
}

void test_unpack_rejects_header_only(void) {
    // no path_len byte
    uint8_t frame[] = { txt_header(ROUTE_TYPE_FLOOD) };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, sizeof(frame), &p));
}

void test_unpack_rejects_truncated_transport_codes(void) {
    // transport route needs 4 bytes of transport codes; only 3 present
    uint8_t frame[] = { txt_header(ROUTE_TYPE_TRANSPORT_FLOOD), 0x11, 0x22, 0x33 };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, sizeof(frame), &p));
}

void test_unpack_rejects_transport_without_path_len(void) {
    // all 4 transport code bytes present, but no path_len byte after them
    uint8_t frame[] = { txt_header(ROUTE_TYPE_TRANSPORT_DIRECT), 0x11, 0x22, 0x33, 0x44 };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, sizeof(frame), &p));
}

void test_unpack_rejects_truncated_path(void) {
    // path_len claims 10 one-byte hops, frame only carries 3
    PM_PathLen pl = { .hop_hash_size = 1, .hops = 10 };
    uint8_t frame[] = { txt_header(ROUTE_TYPE_FLOOD), pack_pathlen(pl), 'a', 'b', 'c' };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, sizeof(frame), &p));
}

void test_unpack_rejects_truncated_multibyte_path(void) {
    // 3 hops of 2-byte hashes = 6 path bytes, frame carries 5
    PM_PathLen pl = { .hop_hash_size = 2, .hops = 3 };
    uint8_t frame[] = { txt_header(ROUTE_TYPE_DIRECT), pack_pathlen(pl), 1, 2, 3, 4, 5 };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, sizeof(frame), &p));
}

// ---- smallest valid frames (boundary for the checks above) ----

void test_unpack_accepts_empty_payload(void) {
    // header + path_len(0 hops), no path, no payload
    PM_PathLen pl = { .hop_hash_size = 1, .hops = 0 };
    uint8_t frame[] = { txt_header(ROUTE_TYPE_FLOOD), pack_pathlen(pl) };
    PM_Packet p;
    memset(&p, 0xAA, sizeof(p));
    TEST_ASSERT_EQUAL_UINT8(0, unpack_exact(frame, sizeof(frame), &p));
    TEST_ASSERT_EQUAL_UINT8(0, p.path_len.hops);
    TEST_ASSERT_EQUAL_UINT8(0, p.payload.len);
}

void test_unpack_accepts_path_filling_frame(void) {
    // path exactly reaches the end of the frame, payload empty
    PM_PathLen pl = { .hop_hash_size = 1, .hops = 3 };
    uint8_t frame[] = { txt_header(ROUTE_TYPE_DIRECT), pack_pathlen(pl), 'a', 'b', 'c' };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(0, unpack_exact(frame, sizeof(frame), &p));
    TEST_ASSERT_EQUAL_UINT8(3, p.path_len.hops);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("abc", p.path, 3);
    TEST_ASSERT_EQUAL_UINT8(0, p.payload.len);
}

void test_unpack_accepts_transport_codes_exact(void) {
    PM_PathLen pl = { .hop_hash_size = 1, .hops = 0 };
    uint8_t frame[] = { txt_header(ROUTE_TYPE_TRANSPORT_FLOOD), 0x11, 0x22, 0x33, 0x44, pack_pathlen(pl) };
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(0, unpack_exact(frame, sizeof(frame), &p));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&frame[1], (uint8_t*)p.transport_codes, 4);
    TEST_ASSERT_EQUAL_UINT8(0, p.payload.len);
}

// ---- too long ----

// Frame of `total` bytes: flood header, 0-hop path_len, then payload bytes 0,1,2,...
static uint8_t *make_payload_frame(uint8_t total) {
    PM_PathLen pl = { .hop_hash_size = 1, .hops = 0 };
    uint8_t *frame = malloc(total);
    frame[0] = txt_header(ROUTE_TYPE_FLOOD);
    frame[1] = pack_pathlen(pl);
    for (int i = 2; i < total; i++) frame[i] = (uint8_t)(i - 2);
    return frame;
}

void test_unpack_accepts_max_payload(void) {
    uint8_t total = 2 + MAX_PAYLOAD_LEN;
    uint8_t *frame = make_payload_frame(total);
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(0, unpack_exact(frame, total, &p));
    TEST_ASSERT_EQUAL_UINT8(MAX_PAYLOAD_LEN, p.payload.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&frame[2], p.payload.data, MAX_PAYLOAD_LEN);
    free(frame);
}

void test_unpack_rejects_payload_one_over_max(void) {
    uint8_t total = 2 + MAX_PAYLOAD_LEN + 1;
    uint8_t *frame = make_payload_frame(total);
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, total, &p));
    free(frame);
}

void test_unpack_rejects_max_trans_unit_payload(void) {
    // the largest frame a uint8_t length allows: 253 bytes of payload
    uint8_t *frame = make_payload_frame(MAX_TRANS_UNIT);
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, MAX_TRANS_UNIT, &p));
    free(frame);
}

void test_unpack_rejects_path_over_max_path_size(void) {
    // path_len claims 40 hops x 2 bytes = 80 path bytes (> MAX_PATH_SIZE 64), and the frame
    // really carries 80 bytes of path + 10 of payload. MeshCore (Dispatcher::tryParsePacket)
    // rejects this; accepting it would mis-split path and payload.
    PM_PathLen pl = { .hop_hash_size = 2, .hops = 40 };
    uint8_t total = 2 + 80 + 10;
    uint8_t frame[2 + 80 + 10];
    memset(frame, 0x5A, sizeof(frame));
    frame[0] = txt_header(ROUTE_TYPE_FLOOD);
    frame[1] = pack_pathlen(pl);
    PM_Packet p;
    TEST_ASSERT_EQUAL_UINT8(1, unpack_exact(frame, total, &p));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack_header_dnr);
    RUN_TEST(test_header_roundtrip);
    RUN_TEST(test_calculate_hash_matches_meshcore);
    RUN_TEST(test_calculate_hash_trace_packet);
    RUN_TEST(test_packet_round_trip);

    RUN_TEST(test_unpack_rejects_empty);
    RUN_TEST(test_unpack_rejects_header_only);
    RUN_TEST(test_unpack_rejects_truncated_transport_codes);
    RUN_TEST(test_unpack_rejects_transport_without_path_len);
    RUN_TEST(test_unpack_rejects_truncated_path);
    RUN_TEST(test_unpack_rejects_truncated_multibyte_path);
    RUN_TEST(test_unpack_accepts_empty_payload);
    RUN_TEST(test_unpack_accepts_path_filling_frame);
    RUN_TEST(test_unpack_accepts_transport_codes_exact);
    RUN_TEST(test_unpack_accepts_max_payload);
    RUN_TEST(test_unpack_rejects_payload_one_over_max);
    RUN_TEST(test_unpack_rejects_max_trans_unit_payload);
    RUN_TEST(test_unpack_rejects_path_over_max_path_size);
    return UNITY_END();
}
