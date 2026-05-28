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

#define OWN_OFF   (MSG_FLASH_SIZE - 0x1000u) // own message: last 4 KB sector
#define OWN_MAX   0x1000u 					 // max size 4k, even though the actual message can only be 250ish bytes. In the future we'll cram some more stuff in here.
#define RECV_OFF  0u                         // received buffer: start of the store. TODO Kinda unneeded, maybe remove.
#define RECV_END  OWN_OFF                    // up to where the own sector begins

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
#define STORE_VERSION 8u

// Default content, written as the first received record on a fresh store.
static const uint8_t README_TXT[] =
	"SynthPass V0.8\n"
	"Collected messages appear in MESSAGES.MD; files appear alongside it.\n";

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
		// Default own item in ME/ (a .txt -> TEXT record): name "OWN.TXT" + greeting.
		{
			static const uint8_t own_name[11] = {'O','W','N',' ',' ',' ',' ',' ','T','X','T'};
			static const char own_default[] = "I'm a bad toaster who forgot to edit their SynthPass message :'[\n";
			uint8_t ownbuf[11 + sizeof(own_default) - 1];
			memcpy(ownbuf, own_name, 11);
			memcpy(ownbuf + 11, own_default, sizeof(own_default) - 1);
			flash_write_record(OWN_OFF, 0, RECORD_TYPE_TEXT, ownbuf, sizeof(ownbuf));
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

	// TEMP (until radio.c wiring): seed a mix of text + file records so the
	// MESSAGES.MD feed and the separate file presentation have something to show.
	if (fresh) {
		static const char m1[] = "Hello from a nearby creature :3\n";
		static const char m2[] = "beep boop :3\n";
		msgstore_received_append(0x1111, RECORD_TYPE_TEXT, (const uint8_t*)m1, sizeof(m1) - 1);
		msgstore_received_append(0x2222, RECORD_TYPE_TEXT, (const uint8_t*)m2, sizeof(m2) - 1);

		// One FILE record: 8.3 name "PROOT.GIF" (last two name chars left spare
		// for a future dedup suffix) followed by a tiny fake payload.
		static const uint8_t file_rec[] = {
			'P','R','O','O','T',' ',' ',' ', 'G','I','F',   // name83[11] -> PROOT.GIF
			0x47,0x49,0x46,0x38,0x39,0x61,                  // fake content "GIF89a"
		};
		// Three with the same name to test name dedup
		// Should come out as PROOT.GIF, PROOT1.GIF, PROOT2.GIF
		msgstore_received_append(0x3333, RECORD_TYPE_FILE, file_rec, sizeof(file_rec));
		msgstore_received_append(0x4040, RECORD_TYPE_FILE, file_rec, sizeof(file_rec));
		msgstore_received_append(0x5050, RECORD_TYPE_FILE, file_rec, sizeof(file_rec));

		// Bulk text so MESSAGES.MD spans more than one 4 KB cluster (exercises edge cases for the cluster boundary).
		static const char bulk[] =
			"The quick brown protogen jumps over the lazy synth. "
			"Boop responsibly. This line pads the feed past one cluster.\n";
		for (uint32_t i = 0; i < 40; i++) {
			msgstore_received_append(0x4444 + i, RECORD_TYPE_TEXT, (const uint8_t*)bulk, sizeof(bulk) - 1);
		}
	}
}

// ---- own item (the user's ME/ file) ----
SynthPassPeerRecord_T msgstore_own(void) {
	return record_view(OWN_OFF);
}

int msgstore_own_present(void) {
	return rec_valid_at(OWN_OFF) && record_view(OWN_OFF).data_length >= 11;
}

void msgstore_own_name(uint8_t out[11]) {
	SynthPassPeerRecord_T r = record_view(OWN_OFF);
	if (rec_valid_at(OWN_OFF) && r.data_length >= 11) memcpy(out, r.data, 11);
	else memset(out, ' ', 11);
}

const uint8_t *msgstore_own_content(void) {
	return record_view(OWN_OFF).data + 11;
}

uint16_t msgstore_own_content_len(void) {
	SynthPassPeerRecord_T r = record_view(OWN_OFF);
	return (rec_valid_at(OWN_OFF) && r.data_length >= 11) ? (uint16_t)(r.data_length - 11) : 0u;
}

__HIGH_CODE
void msgstore_own_set(PeerRecordType_T type, const uint8_t name83[11],
                      const uint8_t *content, uint16_t content_len) {
	if (content_len > MAX_DATA - 11u) content_len = MAX_DATA - 11u;
	uint8_t buf[MAX_DATA];
	memcpy(buf, name83, 11);
	memcpy(buf + 11, content, content_len);
	flash_erase_sector(MSG_FLASH_ADDR + OWN_OFF);
	flash_write_record(OWN_OFF, 0, type, buf, (uint16_t)(11u + content_len));
}

__HIGH_CODE
void msgstore_own_clear(void) {
	flash_erase_sector(MSG_FLASH_ADDR + OWN_OFF);
	flash_write_record(OWN_OFF, 0, RECORD_TYPE_TEXT, (const uint8_t*)"", 0); // empty -> not present
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
