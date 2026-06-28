#ifndef MSGSTORE_H
#define MSGSTORE_H

#include <stdint.h>

// Flash-backed message store: 192 KB reserved at the top of flash (see
// synthpass_ch572.ld). Layout:
//   - sectors 0..45    : received messages (append-only, strictly increasing)
//   - sector 46 (4 KB) : "prox own" message - broadcast on plain proximity
//   - sector 47 (4 KB) : "boop own" message - broadcast when a boop fires
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

// Two separate own slots: PROX is the default (sent on initial detection),
// BOOP is optional (sent during a boop). If BOOP is empty/absent the radio
// falls back to PROX, so users can opt into the second slot incrementally.
typedef enum {
    MSGSTORE_OWN_PROX = 0,
    MSGSTORE_OWN_BOOP = 1,
} MsgstoreOwnKind;

// Validate and erase/seed with default messages if invalid. Call once at boot, before USB comes up (may erase/write flash).
void msgstore_init(void);

// own item: the user's host-editable file in the ME/ folder, one per kind.
// "Empty" (no item -> broadcast nothing) is represented by data_length < 11.
SynthPassPeerRecord_T msgstore_own(MsgstoreOwnKind kind);
int            msgstore_own_present(MsgstoreOwnKind kind);
void           msgstore_own_name(MsgstoreOwnKind kind, uint8_t out[11]);
const uint8_t *msgstore_own_content(MsgstoreOwnKind kind);
uint16_t       msgstore_own_content_len(MsgstoreOwnKind kind);
// Set an own item: type, 8.3 name, and content (name is stored separately).
void msgstore_own_set(MsgstoreOwnKind kind, PeerRecordType_T type, const uint8_t name83[11],
                      const uint8_t *content, uint16_t content_len);
void msgstore_own_clear(MsgstoreOwnKind kind);                       // delete -> empty
// BOOP slot if present, else fall back to PROX. Use for outgoing BOOP_DATA frames.
SynthPassPeerRecord_T msgstore_own_for_boop(void);

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
// Is there an existing received record from peer_id whose content matches `data`
// byte-for-byte? Used by the radio RX path to dedup repeat deliveries of the
// same message. For FILE records the name83 prefix is ignored on both sides
// (the store renames to avoid 8.3 collisions, so the stored name may differ
// from the wire name) -- only the file content has to match.
int msgstore_received_has(uint32_t peer_id, PeerRecordType_T type,
                          const uint8_t *data, uint16_t len);
// Erase all received messages and re-seed the README. Triggered when the host
// deletes MESSAGES.HTM from the volume root.
void msgstore_received_clear(void);

// --- config storage (one dedicated sector) ---
// Read up to `size` bytes of the persisted config blob into `out`. Returns 1
// if a valid record was found, 0 if the sector is uninitialized/corrupt (caller
// should fill its own defaults).
int  msgstore_config_read(void *out, uint16_t size);
// Erase+rewrite the config sector with `size` bytes from `in`.
void msgstore_config_write(const void *in, uint16_t size);

// --- record payload accessors ---
// Copy a FILE record's 8.3 name into out[11] (spaces if not a FILE record).
void msgstore_file_name(SynthPassPeerRecord_T rec, uint8_t out[11]);
// Content view, skipping the 11-byte name prefix on FILE records.
const uint8_t *msgstore_record_content(SynthPassPeerRecord_T rec);
uint16_t       msgstore_record_content_len(SynthPassPeerRecord_T rec);

#endif // MSGSTORE_H
