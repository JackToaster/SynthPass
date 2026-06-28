/*
 * Composite USB device for SynthPass.
 *   - CDC ACM  (EP1/2/3) : debug serial - printf out via _write(), input via
 *                          usb_cdc_getc()
 *   - MSC BOT  (EP5/6)   : FAT16 volume; the boot sector / FAT / root directory
 *                          are synthesized and file contents read from the flash
 *                          message store (msgstore.c). Only OWN.TXT is writable
 *                          (host writes -> msgstore_own_set); writes to anything
 *                          else are accepted but discarded (revert on remount).
 *
 * Built on ch32fun's fsusb (FUSB_USER_HANDLERS); MSC machinery adapted from
 * ch32fun's examples_usb/USBFS/usbfs_msc.
 */
#include "usb.h"
#include "msgstore.h"
#include "config.h"
#include "synthpass.h"   // SYNTHPASS_MAX_MSG_SIZE
#include <string.h>
#include "ch32fun.h"
#include "fsusb.h"

// ---- USB endpoints (must match the descriptors in usb_config.h) ----
#define EP_CDC_IRQ 1   // interrupt IN  - CDC notifications (unused)
#define EP_CDC_OUT 2   // bulk OUT      - CDC debug input
#define EP_CDC_IN  3   // bulk IN       - CDC debug output (printf)
#define EP_MSC_OUT 6   // bulk OUT      - MSC data from host
#define EP_MSC_IN  5   // bulk IN       - MSC data to host


// ============================================================================
// CDC debug serial I/O (printf backend + input)
// ============================================================================
int _write(int fd, const char *buf, int size) {
	(void)fd;
	if(USBFS_SendEndpointNEW(EP_CDC_IN, (uint8_t*)buf, size, /*copy*/1) == -1) { // -1 == busy
		Delay_Ms(1); // wait and try once more
		USBFS_SendEndpointNEW(EP_CDC_IN, (uint8_t*)buf, size, /*copy*/1);
	}
	return size;
}

int putchar(int c) {
	uint8_t single = c;
	if(USBFS_SendEndpointNEW(EP_CDC_IN, &single, 1, /*copy*/1) == -1) { // -1 == busy
		Delay_Ms(1);
		USBFS_SendEndpointNEW(EP_CDC_IN, &single, 1, /*copy*/1);
	}
	return 1;
}

// Last byte received on the CDC endpoint (0 = none); filled by HandleDataOut.
static volatile char cdc_input;

int usb_cdc_getc(void) {
	char c = cdc_input;
	if (c == 0) return -1;
	cdc_input = 0;
	return (unsigned char)c;
}


// ============================================================================
// MSD: read-only FAT16 volume (backed by the flash message store)
// ============================================================================
#define MSC_BLOCK_SIZE      512
#define MSC_TOTAL_SECTORS   0x4000        // reported geometry (8 MB)
#define FILE_CLUSTER        2

// Disk layout constants (based on the boot sector below)
#define START_FAT1      1
#define START_FAT2      17
#define START_ROOT      33  // 1 + 16 + 16
#define START_DATA      65  // 33 + 32 sectors for root dir

// SECTOR 0: FAT16 Boot Sector (BPB) for an 8MB drive
static const uint8_t BootSector[512] = {
	0xEB, 0x3C, 0x90,                               // Jump Instruction
	's','y','n','t','h','p','s','s',                // OEM Name (8)
	0x00, 0x02,                                     // Bytes per sector (512)
	0x08,                                           // Sectors per cluster (4KB clusters)
	0x01, 0x00,                                     // Reserved sectors (1)
	0x02,                                           // Number of FATs (2)
	0x00, 0x02,                                     // Root dir entries (512)
	0x00, 0x40,                                     // Total sectors small (16384 = 8MB)
	0xF8,                                           // Media descriptor
	0x10, 0x00,                                     // Sectors per FAT (16)
	0x3F, 0x00,                                     // Sectors per track
	0xFF, 0x00,                                     // Heads
	0x00, 0x00, 0x00, 0x00,                         // Hidden sectors
	0x00, 0x00, 0x00, 0x00,                         // Large sectors
	0x80,                                           // Drive number
	0x00,                                           // Reserved
	0x29,                                           // Ext boot signature
	0x12, 0x34, 0x56, 0x78,                         // Serial Number
	'S','Y','N','T','H','P','A','S','S',' ',' ',    // Volume Label (11)
	'F', 'A', 'T', '1', '2', ' ', ' ', ' ',         // FS Type (volume has <4085 clusters)
	[510] = 0x55,
	[511] = 0xAA
};

// ---------------------------------------------------------------------------
// Volume layout:
//   root/  MESSAGES.HTM  (HTML feed, synthesized on-the-fly, NEVER stored)
//   root/  ME/          (subdirectory holding the user's single own item)
//   root/  <file>.<ext> (one per RECORD_TYPE_FILE received record)
//   ME/    <own file>   (the host-editable broadcast item; .txt=text else=file)
// TEXT received records are not files - they only appear inside MESSAGES.HTM.
// ---------------------------------------------------------------------------

static uint32_t msc_file_record_count(void) {
	// Cached, keyed by received count (received records are append-only).
	static uint32_t cache_n = 0xFFFFFFFFu, cache_c = 0;
	uint32_t n = msgstore_received_count();
	if (n != cache_n) {
		uint32_t c = 0; MsgStoreIter it; SynthPassPeerRecord_T rec;
		for (msgstore_iter_init(&it); msgstore_iter_next(&it, &rec); )
			if (rec.record_type == RECORD_TYPE_FILE) c++;
		cache_c = c; cache_n = n;
	}
	return cache_c;
}

// Get the k-th FILE record (in receipt order). Returns 1 + fills `*out`, or 0
// if k is out of range. O(n) instead of the O(n^2) random-access loop the previous index-returning version did.
static int msc_filerec(uint32_t k, SynthPassPeerRecord_T *out) {
	MsgStoreIter it; SynthPassPeerRecord_T r;
	uint32_t c = 0;
	for (msgstore_iter_init(&it); msgstore_iter_next(&it, &r); ) {
		if (r.record_type == RECORD_TYPE_FILE) {
			if (c == k) { *out = r; return 1; }
			c++;
		}
	}
	return 0;
}

// ---- MESSAGES.HTM generator (built on the fly; never stored in flash) -------
// A fixed HTML preamble, then one block per record:
//   TEXT: "<p><span class=u>UID</span> CONTENT</p>\n"
//   FILE: "<p><span class=u>UID</span> <img src=NAME.EXT></p>\n"
// UID is ":xx:xx:xx:xx" (little-endian bytes, matching radio.c's printf_uid).
static const char HTML_HEADER[] =
	"<!doctype html><meta charset=utf-8><title>SynthPass</title>"
	"<style>.u{opacity:.5;font-family:monospace}</style>\n";
#define HTML_HEADER_LEN (sizeof(HTML_HEADER) - 1)
#define MD_MAX_BLOCK 320u               // >= 42 + content(<=256) for TEXT; FILE is smaller

static const char MD_HEX[] = "0123456789abcdef";

// Render peer_id as 12 chars ":xx:xx:xx:xx" + NUL into out[13].
static void md_uid(uint32_t peer_id, char *out) {
	char *p = out;
	for (int i = 0; i < 4; i++) {
		uint8_t b = (peer_id >> (8 * i)) & 0xFF;
		*p++ = ':'; *p++ = MD_HEX[b >> 4]; *p++ = MD_HEX[b & 0xF];
	}
	*p = '\0';
}

// Build a FILE record's dotted display name ("PROOT.GIF") into out[13]; len returned.
static uint32_t md_filename(SynthPassPeerRecord_T rec, char *out) {
	uint8_t n83[11];
	msgstore_file_name(rec, n83);
	int nlen = 8; while (nlen > 0 && n83[nlen - 1] == ' ') nlen--;
	int elen = 3; while (elen > 0 && n83[8 + elen - 1] == ' ') elen--;
	uint32_t len = 0;
	for (int i = 0; i < nlen; i++) out[len++] = n83[i];
	if (elen > 0) { out[len++] = '.'; for (int i = 0; i < elen; i++) out[len++] = n83[8 + i]; }
	out[len] = '\0';
	return len;
}

// Formatted feed-block length of a record (no buffer touched).
//   TEXT: <p><span class=u>UID</span> CONTENT</p>\n   = 17+12+8+content+5 = 42+content
//   FILE: <p><span class=u>UID</span> <img src=NAME.EXT></p>\n = 17+12+8+9+fnlen+6 = 52+fnlen
static uint32_t md_block_len(SynthPassPeerRecord_T rec) {
	if (rec.record_type == RECORD_TYPE_FILE) {
		char fn[13];
		uint32_t fnlen = md_filename(rec, fn);
		return 52u + fnlen;
	}
	return 42u + msgstore_record_content_len(rec);
}

static uint32_t md_total_len(void) {
	// Cached, keyed by received count (recomputed O(n) only when it changes).
	static uint32_t cache_n = 0xFFFFFFFFu, cache_t = 0;
	uint32_t n = msgstore_received_count();
	if (n != cache_n) {
		uint32_t total = HTML_HEADER_LEN;
		MsgStoreIter it; SynthPassPeerRecord_T rec;
		for (msgstore_iter_init(&it); msgstore_iter_next(&it, &rec); )
			total += md_block_len(rec);
		cache_t = total; cache_n = n;
	}
	return cache_t;
}

// Clusters occupied by MESSAGES.HTM (>= 1 even when the feed is empty).
static uint16_t msc_md_clusters(void) {
	uint32_t c = (md_total_len() + 4095u) / 4096u;
	return c ? (uint16_t)c : 1u;
}

// Render a record's full feed block into md_block_buf; return its length
// (== md_block_len). MSC BOT is strictly serialized, so a single static buffer
// is safe and keeps it off the read-path stack.
static uint8_t md_block_buf[MD_MAX_BLOCK];
static uint32_t md_build_block(SynthPassPeerRecord_T rec) {
	char uid[13];
	md_uid(rec.peer_id, uid);
	uint8_t *p = md_block_buf;
	memcpy(p, "<p><span class=u>", 17); p += 17;
	memcpy(p, uid, 12);                 p += 12;
	memcpy(p, "</span> ", 8);           p += 8;
	if (rec.record_type == RECORD_TYPE_FILE) {
		char fn[13];
		uint32_t fnlen = md_filename(rec, fn);
		memcpy(p, "<img src=", 9);       p += 9;
		memcpy(p, fn, fnlen);            p += fnlen;
		memcpy(p, "></p>\n", 6);         p += 6;
	} else {
		uint32_t clen = msgstore_record_content_len(rec);
		if (clen > MD_MAX_BLOCK - 50u) clen = MD_MAX_BLOCK - 50u;
		memcpy(p, msgstore_record_content(rec), clen); p += clen;
		memcpy(p, "</p>\n", 5);          p += 5;
	}
	return (uint32_t)(p - md_block_buf);
}

// Produce `len` bytes of the feed starting at byte md_offset. Offset-addressable,
// so it composes with any host chunking; past-EOF stays zero.
// Layout: [HTML_HEADER][record block 0][record block 1]...
static void md_emit(uint32_t md_offset, uint8_t *dst, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) dst[i] = 0;
	uint32_t win_end = md_offset + len;

	// Header
	if (md_offset < HTML_HEADER_LEN) {
		uint32_t take = HTML_HEADER_LEN - md_offset;
		if (take > len) take = len;
		memcpy(dst, HTML_HEADER + md_offset, take);
	}

	// Records, starting at offset HTML_HEADER_LEN
	uint32_t pos = HTML_HEADER_LEN;
	MsgStoreIter it; SynthPassPeerRecord_T rec;
	for (msgstore_iter_init(&it); msgstore_iter_next(&it, &rec); ) {
		uint32_t block_start = pos;
		pos += md_block_len(rec);
		if (block_start >= win_end) break;   // this + later blocks are past the window
		if (pos <= md_offset) continue;      // this block is before the window
		md_build_block(rec);
		uint32_t s = (block_start > md_offset) ? block_start : md_offset;
		uint32_t e = (pos < win_end) ? pos : win_end;
		for (uint32_t b = s; b < e; b++) dst[b - md_offset] = md_block_buf[b - block_start];
	}
}

// ---- FAT + directory + LBA mapping (FAT12; variable multi-cluster layout) ---
// Cluster map (mdc = MESSAGES.HTM cluster count):
//   2 .. 1+mdc   MESSAGES.HTM (chained)    3+mdc       PROX own file (if present)
//   2+mdc        ME/ subdirectory          4+mdc       BOOP own file (if present)
//   5+mdc        SYNTHPAS.INI (always)     6+mdc + k   received FILE record k
// The volume is FAT12 (< 4085 clusters), so FAT entries are 12-bit.
static uint16_t fat_entry(uint16_t c, uint16_t mdc, uint32_t fcount) {
	if (c == 0) return 0xFF8;                 // media descriptor
	if (c == 1) return 0xFFF;                 // reserved
	uint16_t md_last = 1 + mdc;               // MESSAGES.HTM = clusters 2..md_last
	if (c >= 2 && c <= md_last) return (c < md_last) ? (uint16_t)(c + 1) : 0xFFF;
	if (c == 2 + mdc) return 0xFFF;           // ME/ subdir (single)
	// The three own clusters are always marked as allocated, even when the
	// PROX/BOOP slots are empty. Otherwise vfat sees them as free and the
	// host can reuse them for unrelated writes (e.g. SYNTHPAS.INI ends up in
	// BOOP's cluster when BOOP.TXT isn't present).
	if (c == 3 + mdc) return 0xFFF;           // PROX own (reserved)
	if (c == 4 + mdc) return 0xFFF;           // BOOP own (reserved)
	if (c == 5 + mdc) return 0xFFF;           // SYNTHPAS.INI (always synthesized)
	uint16_t fc_first = 6 + mdc;
	if (c >= fc_first && c < fc_first + fcount) return 0xFFF; // received file (single)
	return 0x000;                             // free
}

typedef enum { DA_NONE, DA_MD, DA_ME, DA_OWN_PROX, DA_OWN_BOOP, DA_INI, DA_FILE } da_kind_t;

// Classify a data-area LBA. *base = byte offset of this sector within the file;
// *idx = received-FILE index (DA_FILE only).
static da_kind_t msc_lba_kind(uint32_t lba, uint16_t mdc, uint32_t *base, uint32_t *idx) {
	uint32_t ds = lba - START_DATA;
	uint32_t cluster = 2 + ds / 8, sic = ds % 8;
	if (cluster >= 2 && cluster <= 1u + mdc) { *base = (cluster - 2) * 4096u + sic * 512u; return DA_MD; }
	if (cluster == 2u + mdc) { *base = sic * 512u; return DA_ME; }
	if (cluster == 3u + mdc) { *base = sic * 512u; return DA_OWN_PROX; }
	if (cluster == 4u + mdc) { *base = sic * 512u; return DA_OWN_BOOP; }
	if (cluster == 5u + mdc) { *base = sic * 512u; return DA_INI; }
	uint32_t fc_first = 6u + mdc;
	if (cluster >= fc_first && cluster < fc_first + msc_file_record_count()) {
		*idx = cluster - fc_first; *base = sic * 512u; return DA_FILE;
	}
	return DA_NONE;
}

// Fill a 32-byte directory entry.
static void set_entry(uint8_t entry[32], const uint8_t name11[11], uint8_t attr,
                      uint16_t cluster, uint32_t size) {
	memset(entry, 0, 32);
	memcpy(entry, name11, 11);
	entry[0x0B] = attr;
	entry[0x16] = 0x21; entry[0x18] = 0x21;        // write time/date (arbitrary, non-zero)
	entry[0x1A] = cluster & 0xFF;      entry[0x1B] = cluster >> 8;
	entry[0x1C] = size & 0xFF;         entry[0x1D] = (size >> 8) & 0xFF;
	entry[0x1E] = (size >> 16) & 0xFF; entry[0x1F] = (size >> 24) & 0xFF;
}

// Root directory: 0 = MESSAGES.HTM, 1 = ME/ (dir), 2+ = received FILE records.
static uint32_t msc_root_nfiles(void) { return 2 + msc_file_record_count(); }

static void msc_root_entry(uint32_t slot, uint8_t entry[32], uint16_t mdc) {
	uint8_t name[11]; memset(name, ' ', 11);
	if (slot == 0) {
		memcpy(name, "MESSAGES", 8); memcpy(name + 8, "HTM", 3);
		set_entry(entry, name, 0x01, FILE_CLUSTER, md_total_len());
	} else if (slot == 1) {
		memcpy(name, "ME", 2);
		set_entry(entry, name, 0x10, (uint16_t)(2 + mdc), 0); // directory
	} else {
		SynthPassPeerRecord_T rec;
		if (!msc_filerec(slot - 2, &rec)) { memset(entry, 0, 32); return; } // out of range
		msgstore_file_name(rec, name);
		set_entry(entry, name, 0x01, (uint16_t)(6 + mdc + (slot - 2)), msgstore_record_content_len(rec));
	}
}

// ME/ subdirectory: 0 = ".", 1 = "..", then PROX (if present), BOOP (if
// present), and SYNTHPAS.INI (always synthesized). PROX always lives in cluster
// 3+mdc and BOOP in 4+mdc regardless of which idx they occupy in the listing.
static uint32_t msc_me_nentries(void) {
	uint32_t n = 2 + 1; // . , .. , SYNTHPAS.INI (always)
	if (msgstore_own_present(MSGSTORE_OWN_PROX)) n++;
	if (msgstore_own_present(MSGSTORE_OWN_BOOP)) n++;
	return n;
}

static void msc_me_entry(uint32_t idx, uint8_t entry[32], uint16_t mdc) {
	uint8_t name[11]; memset(name, ' ', 11);
	if (idx == 0) {
		name[0] = '.';
		set_entry(entry, name, 0x10, (uint16_t)(2 + mdc), 0);   // "."  -> self
	} else if (idx == 1) {
		name[0] = '.'; name[1] = '.';
		set_entry(entry, name, 0x10, 0, 0);                     // ".." -> root
	} else {
		int prox = msgstore_own_present(MSGSTORE_OWN_PROX);
		int boop = msgstore_own_present(MSGSTORE_OWN_BOOP);
		uint32_t slot = idx - 2;  // 0-indexed file slot
		if (prox && slot == 0) {
			msgstore_own_name(MSGSTORE_OWN_PROX, name);
			set_entry(entry, name, 0x20, (uint16_t)(3 + mdc), msgstore_own_content_len(MSGSTORE_OWN_PROX));
		} else if (boop && slot == (uint32_t)prox) {
			msgstore_own_name(MSGSTORE_OWN_BOOP, name);
			set_entry(entry, name, 0x20, (uint16_t)(4 + mdc), msgstore_own_content_len(MSGSTORE_OWN_BOOP));
		} else if (slot == (uint32_t)(prox + boop)) {
			// SYNTHPAS.INI, always last so its slot index is deterministic.
			memcpy(name, "SYNTHPASINI", 11);
			set_entry(entry, name, 0x20, (uint16_t)(5 + mdc), config_rendered_len());
		} else {
			memset(entry, 0, 32);
		}
	}
}

// MSC Bulk-Only Transport state machine
typedef enum {
	MSC_IDLE,       // Waiting for CBW
	MSC_DATA_OUT,   // Receiving data from PC (Write)
	MSC_DATA_IN,    // Sending data to PC (Read/Inquiry)
	MSC_SEND_CSW    // Sending Status Wrapper
} msc_state_t;

static volatile msc_state_t msc_state = MSC_IDLE;

// Command Block Wrapper (31 bytes)
struct CBW {
	uint32_t Signature;          // "USBC" (0x43425355)
	uint32_t Tag;
	uint32_t DataTransferLength;
	uint8_t  Flags;
	uint8_t  LUN;
	uint8_t  CBLength;
	uint8_t  CB[16];             // SCSI Command Block
} __attribute__((packed));

// Command Status Wrapper (13 bytes)
struct CSW {
	uint32_t Signature;          // "USBS" (0x53425355)
	uint32_t Tag;
	uint32_t DataResidue;
	uint8_t  Status;             // 0=Pass, 1=Fail, 2=Phase Error
} __attribute__((packed));

static volatile struct CBW cbw;
static volatile struct CSW csw;

static volatile uint32_t msc_current_offset = 0;
static volatile uint32_t msc_bytes_remaining = 0;
static volatile uint32_t msc_write_lba = 0;   // LBA of the in-progress WRITE_10

// ME/ host-write capture. Buffer the ME/ subdir, PROX content, BOOP content,
// and SYNTHPAS.INI content into separate buffers (each cluster has its own LBA
// window); commit from usb_task. me_first_lba locates the ME/ subdir cluster;
// own_prox/own_boop/ini_first_lba are the three writable file clusters.
static volatile uint32_t me_first_lba;
static volatile uint32_t own_prox_first_lba;
static volatile uint32_t own_boop_first_lba;
static volatile uint32_t ini_first_lba;
static uint8_t  me_subdir_buf[512];
static uint8_t  prox_content_buf[256];
static uint8_t  boop_content_buf[256];
static uint8_t  ini_content_buf[384];        // mirrors config.c's rendered_buf size
static uint16_t ini_content_len;             // bytes of valid INI content captured
// Root-dir capture: host deleting MESSAGES.HTM from the volume root marks
// entry 0 with 0xE5; we treat that as "wipe received messages."
static uint8_t  root_dir_buf[512];
static volatile uint8_t root_have_update;
// Atomic-save editors write the new file to a fresh cluster (just past our
// reserved+FILE area) and then rewrite the dir entry to point there. Mirror
// that cluster too so writes still get captured; the commit reads each file's
// current cluster from its dir entry and routes the right buffer over.
static uint8_t  temp_content_buf[384];
static volatile uint16_t temp_content_len;
static volatile uint16_t temp_cluster_num;     // FAT cluster number of the temp slot at WRITE_10 time
static volatile uint32_t temp_first_lba;
static volatile uint8_t  me_have_subdir, prox_have_content, boop_have_content, ini_have_content, temp_have_content, me_commit_pending;

// Decide and send the next IN packet (read / inquiry / etc.), or the CSW.
static void MSC_PrepareDataIn(void) {
	uint8_t msc_response_buffer[64]  __attribute__((aligned(4))) = {0}; // Scratch buffer
	uint32_t len_to_send;
	uint8_t *src_ptr = NULL;
	int is_short_transfer = 0;

	if (msc_state == MSC_SEND_CSW) return;

	// Done sending data -> send CSW.
	if (msc_bytes_remaining == 0) {
		msc_state = MSC_SEND_CSW;
		while(USBFS_SendEndpointNEW(EP_MSC_IN, (uint8_t*)&csw, sizeof(csw), 1) == -1); // -1 == busy
		return;
	}

	switch(cbw.CB[0]) {
	// --- STREAMING DATA COMMANDS ---
	case 0x28: { // READ 10
		uint32_t lba_start = (cbw.CB[2] << 24) | (cbw.CB[3] << 16) | (cbw.CB[4] << 8) | cbw.CB[5];
		uint32_t done = cbw.DataTransferLength - msc_bytes_remaining;
		uint32_t current_lba = lba_start + done / 512;
		uint32_t boff = done % 512;                        // byte offset within the sector
		len_to_send = (msc_bytes_remaining > 64) ? 64 : msc_bytes_remaining;

		uint16_t md_clusters = msc_md_clusters();
		uint32_t nfiles = msc_root_nfiles();

		// --- Boot sector ---
		if (current_lba == 0) {
			memcpy(msc_response_buffer, &BootSector[boff], len_to_send);
		}
		// --- FAT (both copies, all 16 sectors each): emit each byte from
		//     fat_entry() so the variable/chained layout is correct anywhere. ---
		else if ((current_lba >= START_FAT1 && current_lba < START_FAT1 + 16) ||
		         (current_lba >= START_FAT2 && current_lba < START_FAT2 + 16)) {
			uint32_t fat_sec = (current_lba >= START_FAT2) ? (current_lba - START_FAT2)
			                                               : (current_lba - START_FAT1);
			uint32_t fcount = msc_file_record_count();
			for (uint32_t i = 0; i < len_to_send; i++) {
				uint32_t byte = fat_sec * 512u + boff + i;       // byte offset within the FAT
				// FAT12: every 3 bytes pack two 12-bit entries (clusters 2k, 2k+1).
				uint32_t k = byte / 3, r = byte % 3;
				uint16_t e0 = fat_entry((uint16_t)(2u * k),     md_clusters, fcount);
				uint16_t e1 = fat_entry((uint16_t)(2u * k + 1), md_clusters, fcount);
				uint8_t b;
				if (r == 0)      b = e0 & 0xFF;
				else if (r == 1) b = ((e0 >> 8) & 0x0F) | ((e1 & 0x0F) << 4);
				else             b = (e1 >> 4) & 0xFF;
				msc_response_buffer[i] = b;
			}
		}
		// --- Root directory: 32-byte entries, one per logical slot ---
		else if (current_lba >= START_ROOT && current_lba < START_DATA) {
			uint32_t base_entry = (current_lba - START_ROOT) * 16u; // entries before this sector
			for (uint32_t e = 0; e * 32 < len_to_send; e++) {
				uint32_t slot = base_entry + boff / 32 + e;
				if (slot < nfiles) {
					uint8_t entry[32];
					msc_root_entry(slot, entry, md_clusters);
					uint32_t n = (len_to_send - e * 32 < 32) ? (len_to_send - e * 32) : 32;
					memcpy(msc_response_buffer + e * 32, entry, n);
				}
			}
		}
		// --- Data area ---
		else if (current_lba >= START_DATA) {
			uint32_t base = 0, idx = 0;
			da_kind_t kind = msc_lba_kind(current_lba, md_clusters, &base, &idx);
			if (kind == DA_MD) {
				md_emit(base + boff, msc_response_buffer, len_to_send);
			} else if (kind == DA_ME) {
				// ME/ subdir entries live in the cluster's first sector (base 0).
				if (base == 0) {
					uint32_t ne = msc_me_nentries();
					for (uint32_t e = 0; e < ne && e * 32 < boff + len_to_send; e++) {
						uint8_t entry[32];
						msc_me_entry(e, entry, md_clusters);
						for (uint32_t j = 0; j < 32; j++) {
							uint32_t pos = e * 32 + j;
							if (pos >= boff && pos < boff + len_to_send)
								msc_response_buffer[pos - boff] = entry[j];
						}
					}
				}
			} else if (kind == DA_OWN_PROX || kind == DA_OWN_BOOP || kind == DA_INI || kind == DA_FILE) {
				const uint8_t *content; uint32_t clen;
				if (kind == DA_OWN_PROX || kind == DA_OWN_BOOP) {
					MsgstoreOwnKind k = (kind == DA_OWN_PROX) ? MSGSTORE_OWN_PROX : MSGSTORE_OWN_BOOP;
					content = msgstore_own_content(k); clen = msgstore_own_content_len(k);
				} else if (kind == DA_INI) {
					content = config_rendered(); clen = config_rendered_len();
				} else {
					SynthPassPeerRecord_T rec;
					if (!msc_filerec(idx, &rec)) { content = (const uint8_t*)""; clen = 0; }
					else { content = msgstore_record_content(rec); clen = msgstore_record_content_len(rec); }
				}
				for (uint32_t i = 0; i < len_to_send; i++) {
					uint32_t off = base + boff + i;
					if (off < clen) msc_response_buffer[i] = content[off];
				}
			}
		}

		src_ptr = msc_response_buffer;
		break;
	}

	// --- ONE-SHOT COMMANDS (generated headers) ---
	case 0x03: // REQUEST SENSE
		msc_response_buffer[0] = 0x70; // Response Code (Current, Fixed)
		msc_response_buffer[2] = 0x00; // Sense Key (No Sense)
		msc_response_buffer[7] = 0x0A; // Additional Sense Length (10)
		len_to_send = 18;
		src_ptr = msc_response_buffer;
		is_short_transfer = 1;
		break;

	case 0x12: { // INQUIRY
		msc_response_buffer[0] = 0x00; // Direct Access Device
		msc_response_buffer[1] = 0x80; // Removable
		msc_response_buffer[2] = 0x02; // Version
		msc_response_buffer[3] = 0x02; // Format
		msc_response_buffer[4] = 32;   // Additional Length
		memcpy(&msc_response_buffer[8],  "SynthPass", 8);
		memcpy(&msc_response_buffer[16], "Messages        ", 16);
		memcpy(&msc_response_buffer[32], "1.00", 4);

		uint32_t actual_len = 37;
		len_to_send = (msc_bytes_remaining < actual_len) ? msc_bytes_remaining : actual_len;
		src_ptr = msc_response_buffer;
		is_short_transfer = 1;
		break;
	}

	case 0x25: // READ CAPACITY 10
		*(uint32_t*)&msc_response_buffer[0] = __builtin_bswap32(MSC_TOTAL_SECTORS - 1);
		*(uint32_t*)&msc_response_buffer[4] = __builtin_bswap32(MSC_BLOCK_SIZE);
		len_to_send = 8;
		src_ptr = msc_response_buffer;
		is_short_transfer = 1;
		break;

	case 0x1A: // MODE SENSE 6
		memset(msc_response_buffer, 0, 4);
		msc_response_buffer[0] = 3;    // mode data length
		msc_response_buffer[2] = 0x00; // device-specific param: WP=0 (writable, for ME/)
		len_to_send = 4;
		src_ptr = msc_response_buffer;
		is_short_transfer = 1;
		break;

	case 0x5A: // MODE SENSE 10
		memset(msc_response_buffer, 0, 8);
		msc_response_buffer[1] = 6;    // mode data length (low byte)
		msc_response_buffer[3] = 0x00; // device-specific param: WP=0 (writable, for ME/)
		len_to_send = 8;
		src_ptr = msc_response_buffer;
		is_short_transfer = 1;
		break;

	default:
		len_to_send = 0;
		break;
	}

	if (len_to_send > 0 && src_ptr != NULL) {
		while(USBFS_SendEndpointNEW(EP_MSC_IN, src_ptr, len_to_send, 1) == -1); // -1 == busy
		if (is_short_transfer) {
			csw.DataResidue = msc_bytes_remaining - len_to_send;
			msc_bytes_remaining = 0;
		}
		else {
			msc_bytes_remaining -= len_to_send;
			msc_current_offset += len_to_send;
		}
	}
	else {
		msc_state = MSC_SEND_CSW;
		while(USBFS_SendEndpointNEW(EP_MSC_IN, (uint8_t*)&csw, sizeof(csw), 1) == -1); // -1 == busy
	}
}

// Called by the USB IRQ when an IN packet has been taken by the host.
int HandleInRequest(struct _USBState *ctx, int endp, uint8_t *data, int len) {
	(void)ctx; (void)data; (void)len;
	if ((endp == EP_MSC_IN) && (msc_state == MSC_SEND_CSW)) {
		msc_state = MSC_IDLE; // Host grabbed the CSW.
	}
	return 0;
}

// Called by the USB IRQ when an OUT packet has been received from the host.
void HandleDataOut(struct _USBState *ctx, int endp, uint8_t *data, int len) {
	if (endp == 0) {
		ctx->USBFS_SetupReqLen = 0;
	}
	else if (endp == EP_CDC_OUT) {
		// CDC debug input; consumed via usb_cdc_getc() from the main loop.
		if (len > 0) cdc_input = data[0];
	}
	else if (endp == EP_MSC_OUT) {

		// --- 1. IDLE State: Expecting CBW ---
		if (msc_state == MSC_IDLE) {
			if (len != 31) return; // Invalid CBW
			memcpy((void*)&cbw, data, 31);
			if (cbw.Signature != 0x43425355) return; // Invalid Sig

			csw.Signature = 0x53425355;
			csw.Tag = cbw.Tag;
			csw.DataResidue = 0;
			csw.Status = 0;

			switch (cbw.CB[0]) {
			// -- DATA IN COMMANDS --
			case 0x03: // REQUEST SENSE
			case 0x12: // INQUIRY
			case 0x25: // READ CAPACITY
			case 0x1A: // MODE SENSE 6
			case 0x5A: // MODE SENSE 10
			case 0x28: // READ 10
				msc_state = MSC_DATA_IN;
				msc_current_offset = 0;
				msc_bytes_remaining = cbw.DataTransferLength;
				MSC_PrepareDataIn();
				break;

			// -- DATA OUT COMMANDS --
			case 0x2A: // WRITE 10
				msc_state = MSC_DATA_OUT;
				msc_bytes_remaining = cbw.DataTransferLength;
				msc_write_lba = (cbw.CB[2] << 24) | (cbw.CB[3] << 16) | (cbw.CB[4] << 8) | cbw.CB[5];
				me_first_lba       = START_DATA + (uint32_t)msc_md_clusters() * 8u;
				own_prox_first_lba = me_first_lba + 8u;   // cluster 3+mdc (PROX)
				own_boop_first_lba = me_first_lba + 16u;  // cluster 4+mdc (BOOP)
				ini_first_lba      = me_first_lba + 24u;  // cluster 5+mdc (SYNTHPAS.INI)
				// First free cluster after all reserved + FILE records -- this
				// is where vfat puts the temp file of an atomic-rename save.
				temp_cluster_num = (uint16_t)(6u + msc_md_clusters() + msc_file_record_count());
				temp_first_lba   = me_first_lba + 32u + (uint32_t)msc_file_record_count() * 8u;
				break;

			// -- NO DATA COMMANDS --
			case 0x00: // TEST UNIT READY
			case 0x1E: // PREVENT_ALLOW_REMOVAL
			default:
				if(cbw.DataTransferLength > 0 && (cbw.Flags & 0x80)) {
					csw.Status = 1;
				}
				msc_state = MSC_SEND_CSW;
				while(USBFS_SendEndpointNEW(EP_MSC_IN, (uint8_t*)&csw, sizeof(csw), 1) == -1); // -1 == busy
				break;
			}
		}

		// --- 2. DATA OUT State: capture writes into ME/ (own item) ---
		// The ME/ subdir sector carries the file's name/size/presence; any other
		// data-area write is the own file's content (the only writable file). The
		// flash commit is deferred to usb_task(). boot/FAT/root writes are dropped.
		else if (msc_state == MSC_DATA_OUT) {
			uint32_t written = cbw.DataTransferLength - msc_bytes_remaining; // before this packet
			uint32_t lba  = msc_write_lba + written / MSC_BLOCK_SIZE;
			uint32_t boff = written % MSC_BLOCK_SIZE;
			uint32_t n = (len < msc_bytes_remaining) ? len : msc_bytes_remaining;

			if (lba >= me_first_lba && lba < me_first_lba + 8) {
				if (lba == me_first_lba) {            // ME/ subdir entries (first sector)
					for (uint32_t i = 0; i < n; i++) {
						uint32_t off = boff + i;
						if (off < 512) me_subdir_buf[off] = data[i];
					}
					me_have_subdir = 1;
				}
			} else if (lba >= own_prox_first_lba && lba < own_prox_first_lba + 8) {
				// PROX cluster. Other writes (.fseventsd/.Trashes/etc.) miss
				// both windows and get a CSW success with no flash capture.
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < sizeof(prox_content_buf)) prox_content_buf[off] = data[i];
				}
				prox_have_content = 1;
			} else if (lba >= own_boop_first_lba && lba < own_boop_first_lba + 8) {
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < sizeof(boop_content_buf)) boop_content_buf[off] = data[i];
				}
				boop_have_content = 1;
			} else if (lba >= ini_first_lba && lba < ini_first_lba + 8) {
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < sizeof(ini_content_buf)) ini_content_buf[off] = data[i];
				}
				// Track the highest byte the host has written so the commit sees
				// the actual file length, not whatever zeros the host padded the
				// sector with past EOF.
				uint16_t end = (uint16_t)(boff + n);
				if (end > ini_content_len) ini_content_len = end;
				ini_have_content = 1;
			} else if (lba == START_ROOT) {
				// Root dir first sector -- captures host deletion of MESSAGES.HTM.
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < 512) root_dir_buf[off] = data[i];
				}
				root_have_update = 1;
			} else if (lba >= temp_first_lba && lba < temp_first_lba + 8) {
				// Atomic-save temp file. Whichever ME/ entry ends up pointing at
				// this cluster after the dir update gets these bytes (see usb_task).
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < sizeof(temp_content_buf)) temp_content_buf[off] = data[i];
				}
				uint16_t end = (uint16_t)(boff + n);
				if (end > temp_content_len) temp_content_len = end;
				temp_have_content = 1;
			}

			msc_bytes_remaining -= n;
			if (msc_bytes_remaining == 0) {
				if (me_have_subdir || root_have_update) me_commit_pending = 1;  // commit in usb_task (out of IRQ)
				msc_state = MSC_SEND_CSW;
				while(USBFS_SendEndpointNEW(EP_MSC_IN, (uint8_t*)&csw, sizeof(csw), 1) == -1); // -1 == busy
			}
		}
	}
}

// Handle class-specific SETUP requests: CDC line coding + MSC GET_MAX_LUN.
int HandleSetupCustom(struct _USBState *ctx, int setup_code) {
	int ret = -1;
	if ( ctx->USBFS_SetupReqType & USB_REQ_TYP_CLASS ) {
		switch ( setup_code ) {
		case CDC_SET_LINE_CODING:
		case CDC_SET_LINE_CTLSTE:
		case CDC_SEND_BREAK:
			ret = ( ctx->USBFS_SetupReqLen ) ? ctx->USBFS_SetupReqLen : -1;
			break;
		case CDC_GET_LINE_CODING:
			ret = ctx->USBFS_SetupReqLen;
			break;
		case 0xFE: // MSC_GET_MAX_LUN
			ctx->pCtrlPayloadPtr = CTRL0BUFF;
			ctx->pCtrlPayloadPtr[0] = 0; // single LUN (0)
			ret = 1;
			break;
		default:
			ret = 0;
			break;
		}
	}
	else {
		ret = 0; // Go to STALL
	}
	return ret;
}


// ============================================================================
// Public API
// ============================================================================
void usb_init(void) {
	USBFSSetup();
}

// Find the ME/ entry whose 8.3 name begins with `prefix4` (e.g. "PROX" or
// "BOOP"). Returns 1 + fills name83/size/cluster if present, 0 otherwise.
// Skips ./../ long-filename entries and deleted entries.
static int me_parse_slot(const uint8_t *sec, const char *prefix4,
                         uint8_t name83[11], uint32_t *size, uint16_t *cluster) {
	for (int e = 0; e < 16; e++) {
		const uint8_t *en = sec + e * 32;
		uint8_t c0 = en[0], attr = en[0x0B];
		if (c0 == 0x00) break;
		if (c0 == 0xE5) continue;
		if (attr == 0x0F) continue;
		if (attr & 0x18) continue;
		if (memcmp(en, prefix4, 4) != 0) continue;
		memcpy(name83, en, 11);
		*size = en[0x1C] | (en[0x1D] << 8) | (en[0x1E] << 16) | ((uint32_t)en[0x1F] << 24);
		if (cluster) *cluster = (uint16_t)(en[0x1A] | (en[0x1B] << 8));
		return 1;
	}
	return 0;
}

// Is the file's extension ".txt"? Case-insensitive (the |0x20 trick lower-cases
// ASCII letters; non-letters can't match 't'/'x' anyway).
static int name83_is_txt(const uint8_t name83[11]) {
	return (name83[8] | 0x20) == 't'
	    && (name83[9] | 0x20) == 'x'
	    && (name83[10] | 0x20) == 't';
}

void usb_task(void) {
	// Persist a host change to ME/ (flash erase/write must run here, not the IRQ).
	if (me_commit_pending) {
		me_commit_pending = 0;
		// Host deleted MESSAGES.HTM from the volume root: the first byte of
		// entry 0 in root_dir_buf flips to 0xE5. That's the trigger to wipe
		// every received record. The synthesized listing will reappear on the
		// next read (just the README), so the host sees the file return empty.
		if (root_have_update && root_dir_buf[0] == 0xE5) {
			msgstore_received_clear();
		}
		root_have_update = 0;
		// Only parse ME/ slots when the ME/ subdir was actually captured by
		// HandleDataOut this commit cycle. Without this gate, a root-only write
		// (e.g. deleting MESSAGES.HTM) would fall through to the slot loop with
		// a stale/empty me_subdir_buf, me_parse_slot would find nothing, and
		// the "no entry -> clear the slot" branch would wipe PROX.TXT and
		// BOOP.TXT.
		if (me_have_subdir) {
			// Pick the right capture buffer for this file: if the dir entry's
			// cluster matches the firmware's reserved cluster for this slot,
			// use the dedicated buf; if it matches the atomic-save temp cluster,
			// use the temp buf. Otherwise we missed the content -- skip.
			uint16_t mdc = msc_md_clusters();
			uint16_t reserved_prox = (uint16_t)(3 + mdc);
			uint16_t reserved_boop = (uint16_t)(4 + mdc);
			uint16_t reserved_ini  = (uint16_t)(5 + mdc);
			struct slot { MsgstoreOwnKind kind; const char *prefix; uint8_t *cbuf; size_t cbuf_sz;
			              uint8_t have_content; uint16_t reserved_cluster; };
			struct slot slots[2] = {
				{ MSGSTORE_OWN_PROX, "PROX", prox_content_buf, sizeof(prox_content_buf), prox_have_content, reserved_prox },
				{ MSGSTORE_OWN_BOOP, "BOOP", boop_content_buf, sizeof(boop_content_buf), boop_have_content, reserved_boop },
			};
			for (int s = 0; s < 2; s++) {
				uint8_t name83[11]; uint32_t fsize = 0; uint16_t cluster = 0;
				if (me_parse_slot(me_subdir_buf, slots[s].prefix, name83, &fsize, &cluster)) {
					PeerRecordType_T type = name83_is_txt(name83) ? RECORD_TYPE_TEXT : RECORD_TYPE_FILE;
					const uint8_t *src = NULL; uint16_t src_sz = 0;
					if (cluster == slots[s].reserved_cluster && slots[s].have_content) {
						src = slots[s].cbuf;        src_sz = slots[s].cbuf_sz;
					} else if (cluster == temp_cluster_num && temp_have_content) {
						src = temp_content_buf;     src_sz = sizeof(temp_content_buf);
					}
					if (src) {
						uint16_t l = (fsize > SYNTHPASS_MAX_MSG_SIZE) ? SYNTHPASS_MAX_MSG_SIZE : (uint16_t)fsize;
						if (l > src_sz) l = src_sz;
						msgstore_own_set(slots[s].kind, type, name83, src, l);
					} else { // rename of an unchanged file: keep content, update name/type only
						uint16_t clen = msgstore_own_content_len(slots[s].kind);
						uint8_t tmp[256];
						if (clen > sizeof(tmp)) clen = sizeof(tmp);
						memcpy(tmp, msgstore_own_content(slots[s].kind), clen);
						msgstore_own_set(slots[s].kind, type, name83, tmp, clen);
					}
				} else {
					msgstore_own_clear(slots[s].kind);
				}
			}
			// SYNTHPAS.INI: same routing logic, pushed into config.c.
			{
				uint8_t name83[11]; uint32_t fsize = 0; uint16_t cluster = 0;
				if (me_parse_slot(me_subdir_buf, "SYNT", name83, &fsize, &cluster)) {
					const uint8_t *src = NULL; uint16_t src_sz = 0;
					if (cluster == reserved_ini && ini_have_content) {
						src = ini_content_buf;  src_sz = sizeof(ini_content_buf);
					} else if (cluster == temp_cluster_num && temp_have_content) {
						src = temp_content_buf; src_sz = sizeof(temp_content_buf);
					}
					if (src) {
						uint16_t l = (fsize > src_sz) ? src_sz : (uint16_t)fsize;
						config_apply_text(src, l);
					}
				}
			}
		}
		prox_have_content = 0;
		boop_have_content = 0;
		ini_have_content  = 0;
		ini_content_len   = 0;
		temp_have_content = 0;
		temp_content_len  = 0;
		me_have_subdir    = 0;
	}

	// When the host has taken the previous MSC IN packet, send the next.
	if (msc_state == MSC_DATA_IN) {
		MSC_PrepareDataIn();
	}
}

void usb_reset(void) {
	USBFSReset();
}
