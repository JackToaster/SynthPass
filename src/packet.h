#pragma once
#include <stdint.h>


// intentionally chosen to be compatible with MeshCore
#define MAX_PAYLOAD_LEN 184 // biggest payload a packet can carry
#define MAX_PATH_SIZE 64
#define MAX_TRANS_UNIT 255  // biggest entire packet
#define MAX_HASH_SIZE 8     // biggest single hash used in routing


#define ROUTE_TYPE_TRANSPORT_FLOOD   0x00    // flood mode + transport codes
#define ROUTE_TYPE_FLOOD             0x01    // flood mode, needs 'path' to be built up (max 64 bytes)
#define ROUTE_TYPE_DIRECT            0x02    // direct route, 'path' is supplied
#define ROUTE_TYPE_TRANSPORT_DIRECT  0x03    // direct route + transport codes

#define PAYLOAD_TYPE_REQ         0x00    // request (prefixed with dest/src hashes, MAC) (enc data: timestamp, blob)
#define PAYLOAD_TYPE_RESPONSE    0x01    // response to REQ or ANON_REQ (prefixed with dest/src hashes, MAC) (enc data: timestamp, blob)
#define PAYLOAD_TYPE_TXT_MSG     0x02    // a plain text message (prefixed with dest/src hashes, MAC) (enc data: timestamp, text)
#define PAYLOAD_TYPE_ACK         0x03    // a simple ack
#define PAYLOAD_TYPE_ADVERT      0x04    // a node advertising its Identity
#define PAYLOAD_TYPE_GRP_TXT     0x05    // an (unverified) group text message (prefixed with channel hash, MAC) (enc data: timestamp, "name: msg")
#define PAYLOAD_TYPE_GRP_DATA    0x06    // an (unverified) group datagram (prefixed with channel hash, MAC) (enc data: data_type(uint16), data_len, blob)
#define PAYLOAD_TYPE_ANON_REQ    0x07    // generic request (prefixed with dest_hash, ephemeral pub_key, MAC) (enc data: ...)
#define PAYLOAD_TYPE_PATH        0x08    // returned path (prefixed with dest/src hashes, MAC) (enc data: path, extra)
#define PAYLOAD_TYPE_TRACE       0x09    // trace a path, collecting SNR for each hop
#define PAYLOAD_TYPE_MULTIPART   0x0A    // packet is one of a set of packets
#define PAYLOAD_TYPE_CONTROL     0x0B    // a control/discovery packet
//...
#define PAYLOAD_TYPE_RAW_CUSTOM   0x0F    // custom packet as raw bytes, for applications with custom encryption, payloads, etc. SynthPass gets stuffed in here.

#define PAYLOAD_VER_1       0x00   // 1-byte src/dest hashes, 2-byte MAC
#define PAYLOAD_VER_2       0x01   // FUTURE (eg. 2-byte hashes, 4-byte MAC ??)
#define PAYLOAD_VER_3       0x02   // FUTURE
#define PAYLOAD_VER_4       0x03   // FUTURE


typedef struct {
    uint8_t len;
    uint8_t data[MAX_PAYLOAD_LEN];
} PM_Payload;

typedef struct {
    uint8_t hop_hash_size; // how many bytes in each hop hash (top 2 bits, range 1-4)
    uint8_t hops; // how many hops (remaining 6 bits)
} PM_PathLen;

uint8_t pack_pathlen(PM_PathLen len);

PM_PathLen unpack_pathlen(uint8_t len);

typedef struct {
    uint8_t route_type;
    uint8_t payload_type;
    uint8_t version;

    uint8_t do_not_retransmit; // set header to 0xFF for do not retransmit
} PM_Header;

uint8_t pack_header(PM_Header header);

PM_Header unpack_header(uint8_t hdr);


typedef struct {

} PM_Path;

typedef struct {
    PM_Header header;
    PM_PathLen path_len;
    uint16_t transport_codes[2];
    PM_Path path;
    PM_Payload payload;
    int8_t rssi;
} PM_Packet;

PM_Packet unpack_packet(uint8_t *packet, uint8_t len);

// returns: packed packet length
uint8_t pack_packet(PM_Packet *packet, uint8_t *buf);

void packet_calculate_hash(PM_Packet *packet, uint8_t* dest_hash);

uint8_t packet_is_route_flood(PM_Packet *packet);

uint8_t packet_is_route_direct(PM_Packet *packet);

uint8_t packet_has_transport_codes(PM_Packet *packet);

uint8_t packet_get_raw_length(PM_Packet *packet);

void packet_mark_do_not_retransmit(PM_Packet *packet);
