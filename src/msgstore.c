/*
 * Flash-backed message store.
 *
 * 192 KB reserved at the top of flash (0x0000C000..0x0003C000) by
 * synthpass_ch572.ld. Layout:
 *   [0 .. RECV_END)      received messages -- append-only, strictly increasing
 *   [OWN_OFF .. end)     own message       -- rewritable (last 4 KB sector)
 *
 * A record on flash is a 12-byte header (magic[2]="SP", peer_id(u32), record_type(u8), data_length(u16)),
 * then data_length payload bytes, padded to a 4-byte boundary (flash writes in 32-bit words).
 * 
 * Appends only ever touch the (pre-erased) end of the buffer, so there's no
 * read-modify-erase. NOTE: ch5xx_flash_cmd_verify seems unreliable for some
 * reason, so writes are confired by memory-mapped readback instead.
 */
#include "msgstore.h"
#include <string.h>
#include "ch32fun.h"
#include "ch5xx_flash.h"

#define MSG_FLASH_ADDR  (48 * 1024)    // 48 KB: start of the reserved store
#define MSG_FLASH_SIZE  (192 * 1024) // 192 KB reserved for messages

// Three reserved sectors at the top of the message-store region. From the end:
//   - BOOP_OWN_OFF : "boop own" (last sector)
//   - PROX_OWN_OFF : "prox own"
//   - CONFIG_OFF   : host-editable config blob (synthpass.ini)
// All three are 4 KB; received messages occupy everything below.
#define BOOP_OWN_OFF (MSG_FLASH_SIZE - 0x1000u)
#define PROX_OWN_OFF (MSG_FLASH_SIZE - 0x2000u)
#define CONFIG_OFF   (MSG_FLASH_SIZE - 0x3000u)
#define OWN_MAX      0x1000u
#define RECV_OFF     0u
#define RECV_END     CONFIG_OFF                 // received messages end before the config sector

#define CONFIG_MAGIC 0xC0C0FA77u                // sentinel at the start of the config record

// On-flash record header. Packed so the compiler emits byte-wise (unaligned-safe)
// access to peer_id @2 and data_length @8 -- the same reason the previous code
// used memcpy. Reserved bytes are zeroed by flash_write_record.
typedef struct __attribute__((__packed__)) {
	uint8_t  magic[2];     // "SP"
	uint32_t peer_id;
	uint8_t  record_type;  // PeerRecordType_T
	uint8_t  _rsvd1;
	uint16_t data_length;
	uint8_t  _rsvd2[2];
} RecordHeader_T;
#define HDR_SIZE  sizeof(RecordHeader_T)  // 12 bytes

#define MAX_DATA  256u  // cap on a single record's whole payload (FILE = name83[11] + content)

// Store-format version. Stored in the first message (README message). Incrementing invalidates the old store and triggers a flash erase.
#define STORE_VERSION 13u

// Default content, written as the first received record on a fresh store.
static const uint8_t README_TXT[] =
	"SynthPass V0.13. "
	"PROX.TXT goes out on detection, BOOP.TXT goes out on a boop. "
	"Edit SYNTHPAS.INI to tweak tunable parameters.\n";

// Runtime state, (re)computed by msgstore_init().
static uint32_t recv_count;       // number of received records in flash
static uint32_t recv_append_off;  // byte offset of the next free record slot

// ---- read helpers (plain memory-mapped flash access) ----
static const uint8_t *store_ptr(uint32_t off) {
	return (const uint8_t*)(MSG_FLASH_ADDR + off);
}

// checks the magic values at the start of a record
static int rec_valid_at(uint32_t off) {
	const uint8_t *p = store_ptr(off);
	return p[0] == 'S' && p[1] == 'P';
}

// copies a record into RAM and returns it
static SynthPassPeerRecord_T record_view(uint32_t off) {
	const RecordHeader_T *h = (const RecordHeader_T*)store_ptr(off);
	SynthPassPeerRecord_T r;
	r.magic[0]    = h->magic[0];
	r.magic[1]    = h->magic[1];
	r.peer_id     = h->peer_id;          // packed -> compiler emits byte-wise load
	r.record_type = (PeerRecordType_T)h->record_type;
	r.data_length = h->data_length;      // packed -> byte-wise
	r.data        = (const uint8_t*)h + HDR_SIZE;
	return r;
}

// Total on-flash size of the record at off (header + payload, padded to 4 bytes).
static uint32_t rec_total_len(uint32_t off) {
	uint16_t dl = ((const RecordHeader_T*)store_ptr(off))->data_length;
	return (HDR_SIZE + dl + 3u) & ~3u;
}

// Trim n down so buf[0..n) ends at a UTF-8 code-point boundary.
// Drops an incomplete trailing sequence (cap fell inside a multi-byte char).
// Cleans up text sequences with multi-byte characters crossing the size limit.
static uint16_t utf8_trim(const uint8_t *buf, uint16_t n) {
	if (n == 0) return 0;
	// Walk back through continuation bytes (10xxxxxx) to the last code point's lead.
	uint16_t i = n - 1;
	while (i > 0 && (buf[i] & 0xC0) == 0x80) i--;
	// How many bytes does that lead expect?
	uint8_t lead = buf[i];
	uint16_t needed;
	if      ((lead & 0x80) == 0x00) needed = 1; // 0xxxxxxx  ASCII
	else if ((lead & 0xE0) == 0xC0) needed = 2; // 110xxxxx
	else if ((lead & 0xF0) == 0xE0) needed = 3; // 1110xxxx
	else if ((lead & 0xF8) == 0xF0) needed = 4; // 11110xxx
	else                            needed = 0; // malformed lead -> cut it
	// Keep n if the sequence fits in [0, n); else cut at the lead.
	return (needed > 0 && i + needed <= n) ? n : i;
}

// ---- flash writers (MUST run entirely from RAM) ----
// Trying to run flash operations from flash triggers a reset.

// Erase one 4 KB sector (cmd 0x20).
__HIGH_CODE
static void flash_erase_sector(uint32_t addr) {
	ch5xx_flash_rom_open_erase_write();
	ch5xx_flash_rom_addr(0x20, addr);
	ch5xx_flash_rom_wait();
	ch5xx_flash_rom_close();
}

// Program len bytes at addr. Must be 4-byte aligned. Must be erased before writing.
// "memcpy but it goes into flash" 
__HIGH_CODE
static void flash_write(uint32_t addr, const uint8_t *buf, int len) {
	const uint32_t *buf32 = (const uint32_t*)buf;
	int len32 = (len + 3) >> 2;
	ch5xx_flash_rom_open_erase_write();
	while (len32 > 0) {
		ch5xx_flash_rom_addr(0x02, addr);
		while (1) {
			R32_FLASH_DATA = *buf32++;
			for (int i = 4; i > 0; i--) {
				while ((int8_t)R8_FLASH_CTRL < 0);
				R8_FLASH_CTRL = 0x15;
			}
			len32--;
			addr += 4;
			if (len32 == 0 || (addr & 0xff) == 0) break; // stop at 256-byte page boundary
		}
		ch5xx_flash_rom_wait();
	}
	ch5xx_flash_rom_close();
}

// writes a message record into flash
__HIGH_CODE
static void flash_write_record(uint32_t off, uint32_t peer_id, PeerRecordType_T type,
                               const uint8_t *data, uint16_t len) {
	if (len > MAX_DATA) len = MAX_DATA;
	uint8_t buf[HDR_SIZE + MAX_DATA];
	RecordHeader_T *h = (RecordHeader_T*)buf;
	memset(buf, 0, HDR_SIZE);          // zeroes the reserved header bytes too
	h->magic[0]    = 'S';
	h->magic[1]    = 'P';
	h->peer_id     = peer_id;
	h->record_type = (uint8_t)type;
	h->data_length = len;
	memcpy(buf + HDR_SIZE, data, len);
	flash_write(MSG_FLASH_ADDR + off, buf, HDR_SIZE + len);
}

__HIGH_CODE
void msgstore_init(void) {
	// The README record's peer_id (received[0]) doubles as the store-format version. (re)initialize the whole store if it doesn't match
	SynthPassPeerRecord_T r0 = record_view(RECV_OFF);
	int fresh = !(r0.magic[0] == 'S' && r0.magic[1] == 'P') || r0.peer_id != STORE_VERSION;
	if (fresh) {
		// Wipe the whole store, one 4 KB sector at a time.
		// the lib's cmd_erase resets this chip when fetched from flash mid-erase, so we use our own.
		for (uint32_t a = 0; a < MSG_FLASH_SIZE; a += 0x1000u) {
			flash_erase_sector(MSG_FLASH_ADDR + a);
		}
		flash_write_record(RECV_OFF, STORE_VERSION, RECORD_TYPE_TEXT, README_TXT, sizeof(README_TXT) - 1); // README -> received[0] (peer_id = version)
		// Seed PROX.TXT and BOOP.TXT with placeholder greetings so users can see
		// both slots from the host out of the box and edit either in place.
		{
			static const uint8_t prox_name[11] = {'P','R','O','X',' ',' ',' ',' ','T','X','T'};
			static const char prox_default[] = "Edit PROX.TXT in the ME folder to set what other SynthPasses see when you walk by.\n";
			uint8_t buf[11 + sizeof(prox_default) - 1];
			memcpy(buf, prox_name, 11);
			memcpy(buf + 11, prox_default, sizeof(prox_default) - 1);
			flash_write_record(PROX_OWN_OFF, 0, RECORD_TYPE_TEXT, buf, sizeof(buf));
		}
		{
			static const uint8_t boop_name[11] = {'B','O','O','P',' ',' ',' ',' ','T','X','T'};
			static const char boop_default[] = "Edit BOOP.TXT in the ME folder to set what other SynthPasses see when you boop them.\n";
			uint8_t buf[11 + sizeof(boop_default) - 1];
			memcpy(buf, boop_name, 11);
			memcpy(buf + 11, boop_default, sizeof(boop_default) - 1);
			flash_write_record(BOOP_OWN_OFF, 0, RECORD_TYPE_TEXT, buf, sizeof(buf));
		}
	}

	// Populate the received-message state by walking the store with the iterator (stops at the first unwritten / non-valid slot).
	// The iterator's final off IS the next free slot.
	MsgStoreIter scan_it; SynthPassPeerRecord_T scan_r;
	msgstore_iter_init(&scan_it);
	uint32_t count = 0;
	while (msgstore_iter_next(&scan_it, &scan_r)) count++;
	recv_count      = count;
	recv_append_off = scan_it.off;

}

// ---- own items (the user's ME/ files, one per slot) ----
static uint32_t own_off(MsgstoreOwnKind kind) {
	return (kind == MSGSTORE_OWN_BOOP) ? BOOP_OWN_OFF : PROX_OWN_OFF;
}

SynthPassPeerRecord_T msgstore_own(MsgstoreOwnKind kind) {
	return record_view(own_off(kind));
}

int msgstore_own_present(MsgstoreOwnKind kind) {
	uint32_t off = own_off(kind);
	return rec_valid_at(off) && record_view(off).data_length >= 11;
}

void msgstore_own_name(MsgstoreOwnKind kind, uint8_t out[11]) {
	uint32_t off = own_off(kind);
	SynthPassPeerRecord_T r = record_view(off);
	if (rec_valid_at(off) && r.data_length >= 11) memcpy(out, r.data, 11);
	else memset(out, ' ', 11);
}

const uint8_t *msgstore_own_content(MsgstoreOwnKind kind) {
	return record_view(own_off(kind)).data + 11;
}

uint16_t msgstore_own_content_len(MsgstoreOwnKind kind) {
	uint32_t off = own_off(kind);
	SynthPassPeerRecord_T r = record_view(off);
	return (rec_valid_at(off) && r.data_length >= 11) ? (uint16_t)(r.data_length - 11) : 0u;
}

__HIGH_CODE
void msgstore_own_set(MsgstoreOwnKind kind, PeerRecordType_T type, const uint8_t name83[11],
                      const uint8_t *content, uint16_t content_len) {
	if (content_len > MAX_DATA - 11u) content_len = MAX_DATA - 11u;
	if (type == RECORD_TYPE_TEXT) content_len = utf8_trim(content, content_len);
	uint8_t buf[MAX_DATA];
	memcpy(buf, name83, 11);
	memcpy(buf + 11, content, content_len);
	uint32_t off = own_off(kind);
	flash_erase_sector(MSG_FLASH_ADDR + off);
	flash_write_record(off, 0, type, buf, (uint16_t)(11u + content_len));
}

__HIGH_CODE
void msgstore_own_clear(MsgstoreOwnKind kind) {
	uint32_t off = own_off(kind);
	flash_erase_sector(MSG_FLASH_ADDR + off);
	flash_write_record(off, 0, RECORD_TYPE_TEXT, (const uint8_t*)"", 0); // empty -> not present
}

// Returns the BOOP slot if present, else falls back to PROX. Used by the radio
// to pick which own message to send in a BOOP_DATA frame.
SynthPassPeerRecord_T msgstore_own_for_boop(void) {
	if (msgstore_own_present(MSGSTORE_OWN_BOOP)) return msgstore_own(MSGSTORE_OWN_BOOP);
	return msgstore_own(MSGSTORE_OWN_PROX);
}

// ---- config storage ----
// Layout in the CONFIG sector: [u32 magic][caller's bytes...]. The magic
// distinguishes a written record from the all-0xFF erased state.
int msgstore_config_read(void *out, uint16_t size) {
	const uint32_t *magic = (const uint32_t *)(MSG_FLASH_ADDR + CONFIG_OFF);
	if (*magic != CONFIG_MAGIC) return 0;
	memcpy(out, (const uint8_t *)(MSG_FLASH_ADDR + CONFIG_OFF + 4u), size);
	return 1;
}

__HIGH_CODE
void msgstore_config_write(const void *in, uint16_t size) {
	if (size > 248u) size = 248u;            // header(4) + payload <= 252, comfortably under one page
	uint8_t buf[256];
	uint32_t magic = CONFIG_MAGIC;
	memcpy(buf, &magic, 4);
	memcpy(buf + 4, in, size);
	flash_erase_sector(MSG_FLASH_ADDR + CONFIG_OFF);
	flash_write(MSG_FLASH_ADDR + CONFIG_OFF, buf, 4 + size);
}

// ---- received messages ----
uint32_t msgstore_received_count(void) {
	return recv_count;
}

// step through the message store to find the i-th message. O(n) because it's a linked list
SynthPassPeerRecord_T msgstore_received(uint32_t index) {
	MsgStoreIter it;
	SynthPassPeerRecord_T r;
	memset(&r, 0, sizeof(r));   // safe default if index >= count
	msgstore_iter_init(&it);
	for (uint32_t i = 0; i <= index; i++) {
		if (!msgstore_iter_next(&it, &r)) break;
	}
	return r;
}

void msgstore_iter_init(MsgStoreIter *it) { it->off = RECV_OFF; }

int msgstore_iter_next(MsgStoreIter *it, SynthPassPeerRecord_T *out) {
	if (it->off + HDR_SIZE > RECV_END || !rec_valid_at(it->off)) return 0;
	*out = record_view(it->off);
	it->off += rec_total_len(it->off);
	return 1;
}

// Is some existing received FILE record already using this 8.3 name?
static int name83_taken(const uint8_t n83[11]) {
	MsgStoreIter it; SynthPassPeerRecord_T r;
	for (msgstore_iter_init(&it); msgstore_iter_next(&it, &r); ) {
		if (r.record_type == RECORD_TYPE_FILE && r.data_length >= 11 &&
		    memcmp(r.data, n83, 11) == 0)
			return 1;
	}
	return 0;
}

// Write v as a decimal string into out (no NUL). Returns digit count.
static int u_to_dec(char *out, uint32_t v) {
	char rev[10]; int n = 0;
	do { rev[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 10);
	for (int i = 0; i < n; i++) out[i] = rev[n - 1 - i];
	return n;
}

// Make an 8.3 name unique among received FILE records: keep trying
//   <base trimmed to fit><n>.<ext>  for n = 1, 2, 3, ...
// until we find one that isn't taken. The base is truncated so that base+digits
// fits in the 8-char name field; the extension is kept.
static void dedup_name83(uint8_t n83[11]) {
	if (!name83_taken(n83)) return;

	// Significant base-name length (trim trailing spaces from the 8-char name).
	int base_full = 8;
	while (base_full > 0 && n83[base_full - 1] == ' ') base_full--;

	for (uint32_t n = 1; n < 100000000u; n++) {
		char digits[8];
		int dlen = u_to_dec(digits, n);
		if (dlen >= 8) break;                          // suffix too long; give up

		int base = (base_full < 8 - dlen) ? base_full : (8 - dlen);
		uint8_t cand[11];
		memset(cand, ' ', 11);
		memcpy(cand,            n83,      base);       // trimmed base name
		memcpy(cand + base,     digits,   dlen);       // numeric suffix
		memcpy(cand + 8,        n83 + 8,  3);          // keep original extension

		if (!name83_taken(cand)) {
			memcpy(n83, cand, 11);
			return;
		}
	}
}

// add a new message to the store
__HIGH_CODE
int msgstore_received_append(uint32_t peer_id, PeerRecordType_T type,
                             const uint8_t *data, uint16_t len) {
	if (len > MAX_DATA) len = MAX_DATA;
	if (type == RECORD_TYPE_TEXT) len = utf8_trim(data, len);
	uint8_t buf[MAX_DATA];
	if (type == RECORD_TYPE_FILE && len >= 11) {
		memcpy(buf, data, len);
		dedup_name83(buf);   // ensure a unique 8.3 filename among received files
		data = buf;
	}
	uint32_t need = (HDR_SIZE + len + 3u) & ~3u;
	if (recv_append_off + need > RECV_END) {
		return -1; // store full
	}
	flash_write_record(recv_append_off, peer_id, type, data, len);
	recv_append_off += need;
	recv_count++;
	return 0;
}

int msgstore_received_has(uint32_t peer_id, PeerRecordType_T type,
                          const uint8_t *data, uint16_t len) {
	if (len > MAX_DATA) len = MAX_DATA;
	if (type == RECORD_TYPE_TEXT) len = utf8_trim(data, len);
	MsgStoreIter it; SynthPassPeerRecord_T r;
	for (msgstore_iter_init(&it); msgstore_iter_next(&it, &r); ) {
		if (r.peer_id != peer_id) continue;
		if (r.record_type != type) continue;
		if (type == RECORD_TYPE_FILE) {
			// Compare content only -- stored name83 may have been deduped to
			// avoid an 8.3 collision, so the wire name won't necessarily match.
			uint16_t in_cl = (len >= 11) ? (uint16_t)(len - 11) : 0u;
			if (msgstore_record_content_len(r) != in_cl) continue;
			if (memcmp(msgstore_record_content(r), data + 11, in_cl) == 0) return 1;
		} else {
			if (r.data_length != len) continue;
			if (memcmp(r.data, data, len) == 0) return 1;
		}
	}
	return 0;
}

// ---- record payload accessors ----
// For FILE records the payload is name83[11] + content; for TEXT it's all content.
void msgstore_file_name(SynthPassPeerRecord_T rec, uint8_t out[11]) {
	if (rec.record_type == RECORD_TYPE_FILE && rec.data_length >= 11) {
		memcpy(out, rec.data, 11);
	} else {
		memset(out, ' ', 11);
	}
}

const uint8_t *msgstore_record_content(SynthPassPeerRecord_T rec) {
	if (rec.record_type == RECORD_TYPE_FILE && rec.data_length >= 11) {
		return rec.data + 11;
	}
	return rec.data;
}

uint16_t msgstore_record_content_len(SynthPassPeerRecord_T rec) {
	if (rec.record_type == RECORD_TYPE_FILE) {
		return (rec.data_length >= 11) ? (uint16_t)(rec.data_length - 11) : 0u;
	}
	return rec.data_length;
}
