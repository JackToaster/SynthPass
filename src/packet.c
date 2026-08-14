#include "packet.h"
#include "sha256.h"
#include <stdint.h>
#include <string.h>

uint8_t path_len_bytes(PM_PathLen *len) {
    uint16_t len_bytes = len->hop_hash_size * len->hops;
    if(len_bytes > MAX_PATH_SIZE) { return MAX_PATH_SIZE; }
    return len_bytes;
}

uint8_t pack_pathlen(PM_PathLen len) {
    if(len.hop_hash_size * len.hops > MAX_PATH_SIZE || len.hop_hash_size < 1 || len.hop_hash_size > 3 || len.hops > 63) { // hop hash size 4 reserved for future
        // invalid :(
        // todo error?
    }

    return ((len.hop_hash_size - 1) << 6 & 0b11000000) | (len.hops & 0b00111111);
}

PM_PathLen unpack_pathlen(uint8_t len) {
    return (PM_PathLen){
        .hop_hash_size = ((len >> 6) & 0b11) + 1,
        .hops = len & 0b00111111
    };
}

uint8_t pack_header(PM_Header header) {
    if(header.do_not_retransmit) return 0xFF;
    if(header.route_type > 0b11 || header.payload_type > 0b1111 || header.version > 0){
        // invalid :(
        // todo error?
    }
    return (header.route_type & 0b11) | (header.payload_type & 0b1111) << 2 | (header.version & 0b11) << 6;
}

PM_Header unpack_header(uint8_t hdr) {
    if(hdr == 0xFF) {
        return (PM_Header){
            .do_not_retransmit = 1
        };
    }
    return (PM_Header){
        .route_type = hdr & 0b11,
        .payload_type = (hdr >> 2) & 0b1111,
        .version = (hdr >> 6) & 0b11,
        .do_not_retransmit = 0
    };
}

void unpack_packet(uint8_t *packet, uint8_t len, PM_Packet *into) {
    uint8_t i = 0;
    into->header = unpack_header(packet[i++]);
    if(packet_has_transport_codes(into)) {
        memcpy(&into->transport_codes, &packet[i], sizeof(into->transport_codes));
        i += sizeof(into->transport_codes);
    }

    into->path_len = unpack_pathlen(packet[i++]);
    uint8_t pl_b = path_len_bytes(&into->path_len);

    memcpy(&into->path, &packet[i], pl_b);
    i += pl_b;
    uint8_t payload_len = len - i;
    into->payload.len = payload_len;
    memcpy(&into->payload.data, &packet[i], payload_len);
}

// returns: packed packet length
uint8_t pack_packet(PM_Packet *packet, uint8_t *buf) {
    uint8_t len = packet_get_raw_length(packet);
    uint8_t i = 0;
    buf[i++] = pack_header(packet->header);
    
    if(packet_has_transport_codes(packet)) {
        memcpy(&buf[i], &packet->transport_codes, sizeof(packet->transport_codes));
        i += sizeof(packet->transport_codes);
    }

    buf[i++] = pack_pathlen(packet->path_len);
    
    uint8_t path_len = path_len_bytes(&packet->path_len);
    memcpy(&buf[i], packet->path, path_len);
    i += path_len;

    if(i + packet->payload.len < i) {
        // overflow!! don't write outside buffer
        return MAX_TRANS_UNIT;
        // todo error handler?
    }

    memcpy(&buf[i], &packet->payload.data, packet->payload.len);
    return len;
}

// dest hash length 32 bytes
void packet_calculate_hash(PM_Packet *packet, uint8_t* dest_hash) {
    // meshcore impl
//   SHA256 sha;
//   uint8_t t = getPayloadType();
//   sha.update(&t, 1);
//   if (t == PAYLOAD_TYPE_TRACE) {
//     sha.update(&path_len, sizeof(path_len));   // CAVEAT: TRACE packets can revisit same node on return path
//   }
//   sha.update(payload, payload_len);
//   sha.finalize(hash, MAX_HASH_SIZE);

    sha256 sha;
    sha256_init(&sha);
    uint8_t t = packet->header.payload_type;
    sha256_append(&sha, &t, 1);

    if(t == PAYLOAD_TYPE_TRACE) {
        uint8_t pathlen = pack_pathlen(packet->path_len);
        sha256_append(&sha, &pathlen, 1); // CAVEAT: TRACE packets can revisit same node on return path
        uint8_t zero = 0; // sizeof(path_len) is 2 in meshcore!
        sha256_append(&sha, &zero, 1);
    }
    sha256_append(&sha, packet->payload.data, packet->payload.len);

    sha256_finalize_bytes(&sha, dest_hash, 8);
}

uint8_t packet_is_route_flood(PM_Packet *packet) {
    return packet->header.route_type == ROUTE_TYPE_FLOOD || packet->header.route_type == ROUTE_TYPE_TRANSPORT_FLOOD;
}

uint8_t packet_is_route_direct(PM_Packet *packet) {
    return packet->header.route_type == ROUTE_TYPE_DIRECT || packet->header.route_type == ROUTE_TYPE_TRANSPORT_DIRECT;
}

uint8_t packet_has_transport_codes(PM_Packet *packet) {
    return packet->header.route_type == ROUTE_TYPE_TRANSPORT_DIRECT || packet->header.route_type == ROUTE_TYPE_TRANSPORT_FLOOD;
}

uint8_t packet_get_raw_length(PM_Packet *packet) {
    return 2 + (packet_has_transport_codes(packet) ? 4 : 0) + path_len_bytes(&packet->path_len) + packet->payload.len;
}

void packet_mark_do_not_retransmit(PM_Packet *packet) {
    packet->header.do_not_retransmit = 1;
}
