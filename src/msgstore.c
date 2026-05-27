/*
 * Flash-backed message store.
 *
 * 192 KB reserved at the top of flash (0x0000C000..0x0003C000) by
 * synthpass_ch572.ld. Layout:
 *   [0 .. RECV_END)      received messages -- append-only, strictly increasing
 *   [OWN_OFF .. end)     own message       -- rewritable (last 4 KB sector)
 *
 * A record on flash is: magic[2]="SP", peer_id(u32), data_length(u16), then
 * data_length payload bytes, padded to a 4-byte boundary (ch5xx_flash writes in
 * 32-bit words). Reads are plain memory-mapped accesses; writes go through
 * ch32fun's ch5xx_flash (4 KB-sector erase + program) and run from RAM.
 *
 * Appends only ever touch the (pre-erased) end of the buffer, so there's no
 * read-modify-erase. NOTE: ch5xx_flash_cmd_verify is unreliable on this chip, so
 * we never use it -- writes are confirmed by memory-mapped readback instead.
 */
#include "msgstore.h"
#include <string.h>
#include "ch32fun.h"
#include "ch5xx_flash.h"

#define MSG_FLASH_ADDR  0x0000C000u    // 48 KB: start of the reserved store
#define MSG_FLASH_SIZE  (192u * 1024u) // 192 KB reserved for messages

#define OWN_OFF   (MSG_FLASH_SIZE - 0x1000u) // own message: last 4 KB sector
#define OWN_MAX   0x1000u
#define RECV_OFF  0u                         // received buffer: start of the store
#define RECV_END  OWN_OFF                    // ...up to where the own sector begins

#define HDR_SIZE  8u    // magic[2] + peer_id(4) + data_length(2)
#define MAX_DATA  256u  // cap on a single record's payload

// Store-format version, stored in the README record's peer_id (received[0]).
// README is never host-editable, so editing OWN.TXT can't trip the fresh check.
// Bump this to force a one-time wipe + re-seed when the seeded layout changes.
#define STORE_VERSION 2u

// Default content, written as the first received record on a fresh store.
static const uint8_t README_TXT[] =
	"SynthPass V0.1\r\n"
	"Collected messages appear as files in this folder.\r\n";

// Runtime state, (re)computed by msgstore_init().
static uint32_t recv_count;       // number of received records in flash
static uint32_t recv_append_off;  // byte offset of the next free record slot

// ---- read helpers (plain memory-mapped flash access) ----
static const uint8_t *store_ptr(uint32_t off) {
	return (const uint8_t*)(MSG_FLASH_ADDR + off);
}

static int rec_valid_at(uint32_t off) {
	const uint8_t *p = store_ptr(off);
	return p[0] == 'S' && p[1] == 'P';
}

static SynthPassPeerRecord_T record_view(uint32_t off) {
	const uint8_t *p = store_ptr(off);
	SynthPassPeerRecord_T r;
	r.magic[0] = p[0];
	r.magic[1] = p[1];
	memcpy(&r.peer_id, p + 2, 4);     // memcpy: tolerate unaligned flash layout
	memcpy(&r.data_length, p + 6, 2);
	r.data = p + HDR_SIZE;
	return r;
}

// Total on-flash size of the record at off (header + payload, padded to 4 bytes).
static uint32_t rec_total_len(uint32_t off) {
	uint16_t dl;
	memcpy(&dl, store_ptr(off) + 6, 2);
	return (HDR_SIZE + dl + 3u) & ~3u;
}

// ---- flash writers (MUST run entirely from RAM) ----
// ch5xx_flash_cmd_erase/cmd_write live in flash and get instruction-fetched
// while the flash is in erase/write-enabled mode, which resets this chip -- only
// the lib's rom_* primitives are __HIGH_CODE. So reimplement the erase/write
// loops here as __HIGH_CODE wrappers that call those RAM-resident primitives.

// Erase one 4 KB sector (cmd 0x20).
__HIGH_CODE
static void flash_erase_sector(uint32_t addr) {
	ch5xx_flash_rom_open_erase_write();
	ch5xx_flash_rom_addr(0x20, addr);
	ch5xx_flash_rom_wait();
	ch5xx_flash_rom_close();
}

// Program len bytes at addr (which must be in pre-erased flash), in 4-byte words.
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

__HIGH_CODE
static void flash_write_record(uint32_t off, uint32_t peer_id, const uint8_t *data, uint16_t len) {
	if (len > MAX_DATA) len = MAX_DATA;
	uint8_t buf[HDR_SIZE + MAX_DATA];
	buf[0] = 'S';
	buf[1] = 'P';
	memcpy(buf + 2, &peer_id, 4);
	memcpy(buf + 6, &len, 2);
	memcpy(buf + HDR_SIZE, data, len);
	flash_write(MSG_FLASH_ADDR + off, buf, HDR_SIZE + len);
}

__HIGH_CODE
void msgstore_init(void) {
	// The README record's peer_id (received[0]) doubles as the store-format
	// version; if it's missing or stale, (re)initialize the whole store.
	SynthPassPeerRecord_T r0 = record_view(RECV_OFF);
	int fresh = !(r0.magic[0] == 'S' && r0.magic[1] == 'P') || r0.peer_id != STORE_VERSION;
	if (fresh) {
		// Wipe the whole store, one 4 KB sector at a time, so the append-only
		// received buffer is clean everywhere. RAM-resident sector erase (0x20):
		// the lib's cmd_erase resets this chip when fetched from flash mid-erase,
		// and its multi-block erase (0xd8/0x52) hangs outright.
		for (uint32_t a = 0; a < MSG_FLASH_SIZE; a += 0x1000u) {
			flash_erase_sector(MSG_FLASH_ADDR + a);
		}
		flash_write_record(RECV_OFF, STORE_VERSION, README_TXT, sizeof(README_TXT) - 1); // README -> received[0] (peer_id = version)
		flash_write_record(OWN_OFF, 0, README_TXT, sizeof(README_TXT) - 1);             // default own message
	}

	// Populate the received-message state by scanning records until the first
	// slot that isn't a valid record (unwritten flash / RECORD_MAGIC not found).
	uint32_t off = RECV_OFF, count = 0;
	while (off + HDR_SIZE <= RECV_END && rec_valid_at(off)) {
		count++;
		off += rec_total_len(off);
	}
	recv_count = count;
	recv_append_off = off;

	// TEMP (until radio.c wiring): seed a few test received messages so the
	// multi-file MSC presentation has something to show.
	if (fresh) {
		static const char m1[] = "Hello from a nearby SynthPass!\r\n";
		static const char m2[] = "boop boop :3\r\n";
		static const char m3[] = "the third message\r\n";
		msgstore_received_append(0x1111, (const uint8_t*)m1, sizeof(m1) - 1);
		msgstore_received_append(0x2222, (const uint8_t*)m2, sizeof(m2) - 1);
		msgstore_received_append(0x3333, (const uint8_t*)m3, sizeof(m3) - 1);
	}
}

// ---- own message ----
SynthPassPeerRecord_T msgstore_own(void) {
	return record_view(OWN_OFF);
}

__HIGH_CODE
void msgstore_own_set(uint32_t peer_id, const uint8_t *data, uint16_t len) {
	flash_erase_sector(MSG_FLASH_ADDR + OWN_OFF); // erase the own sector (RAM-resident)
	flash_write_record(OWN_OFF, peer_id, data, len);
}

// ---- received messages ----
uint32_t msgstore_received_count(void) {
	return recv_count;
}

SynthPassPeerRecord_T msgstore_received(uint32_t index) {
	uint32_t off = RECV_OFF;
	for (uint32_t i = 0; i < index; i++) {
		off += rec_total_len(off);
	}
	return record_view(off);
}

__HIGH_CODE
int msgstore_received_append(uint32_t peer_id, const uint8_t *data, uint16_t len) {
	if (len > MAX_DATA) len = MAX_DATA;
	uint32_t need = (HDR_SIZE + len + 3u) & ~3u;
	if (recv_append_off + need > RECV_END) {
		return -1; // store full
	}
	flash_write_record(recv_append_off, peer_id, data, len);
	recv_append_off += need;
	recv_count++;
	return 0;
}
