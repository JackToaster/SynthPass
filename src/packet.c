#include "packet.h"
#include "sha256.h"

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
    PM_Header header;
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

PM_Packet unpack_packet(uint8_t *packet, uint8_t len) {
    // todo
}

// returns: packed packet length
uint8_t pack_packet(PM_Packet *packet, uint8_t *buf) {
    // todo
}

void packet_calculate_hash(PM_Packet *packet, uint8_t* dest_hash) {
  SHA256 sha;
  uint8_t t = getPayloadType();
  sha.update(&t, 1);
  if (t == PAYLOAD_TYPE_TRACE) {
    sha.update(&path_len, sizeof(path_len));   // CAVEAT: TRACE packets can revisit same node on return path
  }
  sha.update(payload, payload_len);
  sha.finalize(hash, MAX_HASH_SIZE);}

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
    return 2 + (packet_has_transport_codes(packet) ? 4 : 0) + (packet->path_len.hop_hash_size * packet->path_len.hops) + packet->payload.len;
}

void packet_mark_do_not_retransmit(PM_Packet *packet) {
    packet->header.do_not_retransmit = 1;
}
