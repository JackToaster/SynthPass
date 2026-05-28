#ifndef MSGSTORE_H
#define MSGSTORE_H

#include <stdint.h>

// Flash-backed message store: 192 KB reserved at the top of flash (see
// synthpass_ch572.ld). Layout:
//   - sector 0 (4 KB) : "own message" - the record we broadcast (rewritable)
//   - sectors 1..47   : received messages - append-only, strictly increasing
//
// On flash a record is a 12-byte header (magic[2]="SP", peer_id, record_type, data_length)
// followed by data_length payload bytes, padded to a 4 bytes.
//
// SynthPassPeerRecord_T is the in-RAM view.

#define RECORD_MAGIC "SP"

typedef enum {
    RECORD_TYPE_TEXT = 0,  // UTF-8 text; glommed into the MESSAGES.MD feed.
                           // Stored bytes are trimmed to a code-point boundary
                           // so a truncation never leaves a half-character.
    RECORD_TYPE_FILE = 1,  // arbitrary bytes, stuck in a markdown image tag
} PeerRecordType_T;

typedef struct {
    uint8_t magic[2];
    uint32_t peer_id;
    PeerRecordType_T record_type;
    uint16_t data_length;
    const uint8_t* data;
} SynthPassPeerRecord_T;

// Payload layout of a RECORD_TYPE_FILE record: a raw 8.3 directory name 
// (8-char name + 3-char extension, space-padded, uppercase, no dot) followed by the file content.
// Max length names will have their last character(s) overwritten to deduplicate file names, if needed.
typedef struct {
    uint8_t name83[11]; // 8.3 name, e.g. {'P','R','O','O','T',' ',' ',' ','G','I','F'} -> PROOT.GIF
    uint8_t data[];     // file content
} FileRecordHeader_T;

// Validate and erase/seed with default messages if invalid. Call once at boot, before USB comes up (may erase/write flash).
void msgstore_init(void);

// own item: the user's single file in the ME/ folder (host-editable)
// "Empty" (no item -> broadcast nothing) is represented by data_length < 11.
SynthPassPeerRecord_T msgstore_own(void);
int            msgstore_own_present(void);           // 1 if ME/ holds a file
void           msgstore_own_name(uint8_t out[11]);   // 8.3 name (spaces if empty)
const uint8_t *msgstore_own_content(void);           // content (after the name)
uint16_t       msgstore_own_content_len(void);
// Set the own item: type, 8.3 name, and content (name is stored separately).
void msgstore_own_set(PeerRecordType_T type, const uint8_t name83[11],
                      const uint8_t *content, uint16_t content_len);
void msgstore_own_clear(void);                       // delete -> empty

// --- received messages: append-only, in order of receipt ---
uint32_t msgstore_received_count(void);
SynthPassPeerRecord_T msgstore_received(uint32_t index);  // index in [0, count)
// Sequential iteration (O(1) per step; avoids the O(n^2) of repeated
// msgstore_received(i) when walking the whole received buffer).
typedef struct { uint32_t off; } MsgStoreIter;
void msgstore_iter_init(MsgStoreIter *it);
int  msgstore_iter_next(MsgStoreIter *it, SynthPassPeerRecord_T *out);
// Append a received record to the end of the buffer. Returns 0 on success,
// -1 if the store is full. For RECORD_TYPE_FILE, `data` must be name83[11] +
// content (see FileRecordHeader_T) and `len` covers both.
int msgstore_received_append(uint32_t peer_id, PeerRecordType_T type,
                             const uint8_t *data, uint16_t len);

// --- record payload accessors ---
// Copy a FILE record's 8.3 name into out[11] (spaces if not a FILE record).
void msgstore_file_name(SynthPassPeerRecord_T rec, uint8_t out[11]);
// Content view, skipping the 11-byte name prefix on FILE records.
const uint8_t *msgstore_record_content(SynthPassPeerRecord_T rec);
uint16_t       msgstore_record_content_len(SynthPassPeerRecord_T rec);

#endif // MSGSTORE_H
