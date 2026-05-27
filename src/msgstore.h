#ifndef MSGSTORE_H
#define MSGSTORE_H

#include <stdint.h>

// Flash-backed message store: 192 KB reserved at the top of flash (see
// synthpass_ch572.ld). Layout:
//   - sector 0 (4 KB) : "own message" -- the record we broadcast (rewritable)
//   - sectors 1..47   : received messages -- append-only, strictly increasing
//
// On flash a record is stored as the header fields below packed
// (magic[2]="SP", peer_id, data_length) followed by data_length bytes of data, padded to a 4-byte boundary.
// SynthPassPeerRecord_T is the in-RAM view of a record; its `data` points into the memory-mapped flash.

#define RECORD_MAGIC "SP"

typedef enum {
    RECORD_TYPE_TEXT = 0u8;
    RECORD_TYPE_FILE = 1u8;
} PeerRecordType_T;

typedef struct {
    uint8_t magic[2];
    uint32_t peer_id;
    PeerRecordType_T record_type;
    uint16_t data_length;
    const uint8_t* data;
} SynthPassPeerRecord_T;

typedef struct {
    uint8_t filename_ext[9]; // 8 chars filename, 3 chars extension. Padd with nulls. E.g. "PROOT\0\0\0GIF" -> PROOT.GIF. On receipt of a repeat filename, numbers appended to end of file (PROOT01.PNG).
    uint8_t data[]; // remaining data
} FileRecordHeader_T;

// Validate the store and seed the default own message on first use.
// Call once at boot, before USB comes up (may erase/write flash).
void msgstore_init(void);

// --- own message: the record we broadcast (a host-visible writable file) ---
SynthPassPeerRecord_T msgstore_own(void);
void msgstore_own_set(uint32_t peer_id, const uint8_t *data, uint16_t len);

// --- received messages: append-only, in order of receipt ---
uint32_t msgstore_received_count(void);
SynthPassPeerRecord_T msgstore_received(uint32_t index);  // index in [0, count)
// Append a received record to the end of the buffer. Returns 0 on success,
// -1 if the store is full.
int msgstore_received_append(uint32_t peer_id, const uint8_t *data, uint16_t len);

#endif // MSGSTORE_H
