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
//   root/  MESSAGES.MD  (markdown feed, synthesized on-the-fly, NEVER stored)
//   root/  ME/          (subdirectory holding the user's single own item)
//   root/  <file>.<ext> (one per RECORD_TYPE_FILE received record)
//   ME/    <own file>   (the host-editable broadcast item; .txt=text else=file)
// TEXT received records are not files - they only appear inside MESSAGES.MD.
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

// ---- MESSAGES.MD generator (built on the fly; never stored in flash) -------
// Per record: "<uid>: <text>\n----\n"  or  "<uid>: ![<uid>](<NAME.EXT>)\n----\n"
// UID is ":xx:xx:xx:xx" (little-endian bytes, matching radio.c's printf_uid).
#define MD_SEP_LEN   6u                 // "\n----\n"
#define MD_MAX_BLOCK 288u               // >= uid(12)+": "(2)+content(<=256)+"\n----\n"(6)

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
static uint32_t md_block_len(SynthPassPeerRecord_T rec) {
	if (rec.record_type == RECORD_TYPE_FILE) {
		char fn[13];
		uint32_t fnlen = md_filename(rec, fn);
		// uid + ": " + "![" + uid + "](" + name + ")" + sep
		return 12u + 2u + 2u + 12u + 2u + fnlen + 1u + MD_SEP_LEN;
	}
	return 12u + 2u + msgstore_record_content_len(rec) + MD_SEP_LEN;
}

static uint32_t md_total_len(void) {
	// Cached, keyed by received count (recomputed O(n) only when it changes).
	static uint32_t cache_n = 0xFFFFFFFFu, cache_t = 0;
	uint32_t n = msgstore_received_count();
	if (n != cache_n) {
		uint32_t total = 0; MsgStoreIter it; SynthPassPeerRecord_T rec;
		for (msgstore_iter_init(&it); msgstore_iter_next(&it, &rec); )
			total += md_block_len(rec);
		cache_t = total; cache_n = n;
	}
	return cache_t;
}

// Clusters occupied by MESSAGES.MD (>= 1 even when the feed is empty).
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
	memcpy(p, uid, 12); p += 12;
	*p++ = ':'; *p++ = ' ';
	if (rec.record_type == RECORD_TYPE_FILE) {
		char fn[13];
		uint32_t fnlen = md_filename(rec, fn);
		*p++ = '!'; *p++ = '[';
		memcpy(p, uid, 12); p += 12;
		*p++ = ']'; *p++ = '(';
		memcpy(p, fn, fnlen); p += fnlen;
		*p++ = ')';
	} else {
		uint32_t clen = msgstore_record_content_len(rec);
		if (clen > MD_MAX_BLOCK - 20u) clen = MD_MAX_BLOCK - 20u; // never overflow the buffer
		memcpy(p, msgstore_record_content(rec), clen); p += clen;
	}
	*p++ = '\n'; *p++ = '-'; *p++ = '-'; *p++ = '-'; *p++ = '-'; *p++ = '\n';
	return (uint32_t)(p - md_block_buf);
}

// Produce `len` bytes of the feed starting at byte md_offset. Offset-addressable,
// so it composes with any host chunking; past-EOF stays zero.
static void md_emit(uint32_t md_offset, uint8_t *dst, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) dst[i] = 0;
	uint32_t pos = 0, win_end = md_offset + len;
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
// Cluster map (mdc = MESSAGES.MD cluster count):
//   2 .. 1+mdc   MESSAGES.MD (chained)    3+mdc       the own item's file (if present)
//   2+mdc        ME/ subdirectory          4+mdc + k   received FILE record k
// The volume is FAT12 (< 4085 clusters), so FAT entries are 12-bit.
static uint16_t fat_entry(uint16_t c, uint16_t mdc, uint32_t fcount) {
	if (c == 0) return 0xFF8;                 // media descriptor
	if (c == 1) return 0xFFF;                 // reserved
	uint16_t md_last = 1 + mdc;               // MESSAGES.MD = clusters 2..md_last
	if (c >= 2 && c <= md_last) return (c < md_last) ? (uint16_t)(c + 1) : 0xFFF;
	if (c == 2 + mdc) return 0xFFF;           // ME/ subdir (single)
	if (c == 3 + mdc) return msgstore_own_present() ? 0xFFF : 0x000; // own file
	uint16_t fc_first = 4 + mdc;
	if (c >= fc_first && c < fc_first + fcount) return 0xFFF; // received file (single)
	return 0x000;                             // free
}

typedef enum { DA_NONE, DA_MD, DA_ME, DA_OWN, DA_FILE } da_kind_t;

// Classify a data-area LBA. *base = byte offset of this sector within the file;
// *idx = received-FILE index (DA_FILE only).
static da_kind_t msc_lba_kind(uint32_t lba, uint16_t mdc, uint32_t *base, uint32_t *idx) {
	uint32_t ds = lba - START_DATA;
	uint32_t cluster = 2 + ds / 8, sic = ds % 8;
	if (cluster >= 2 && cluster <= 1u + mdc) { *base = (cluster - 2) * 4096u + sic * 512u; return DA_MD; }
	if (cluster == 2u + mdc) { *base = sic * 512u; return DA_ME; }
	if (cluster == 3u + mdc) { *base = sic * 512u; return DA_OWN; }
	uint32_t fc_first = 4u + mdc;
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

// Root directory: 0 = MESSAGES.MD, 1 = ME/ (dir), 2+ = received FILE records.
static uint32_t msc_root_nfiles(void) { return 2 + msc_file_record_count(); }

static void msc_root_entry(uint32_t slot, uint8_t entry[32], uint16_t mdc) {
	uint8_t name[11]; memset(name, ' ', 11);
	if (slot == 0) {
		memcpy(name, "MESSAGES", 8); memcpy(name + 8, "MD", 2);
		set_entry(entry, name, 0x01, FILE_CLUSTER, md_total_len());
	} else if (slot == 1) {
		memcpy(name, "ME", 2);
		set_entry(entry, name, 0x10, (uint16_t)(2 + mdc), 0); // directory
	} else {
		SynthPassPeerRecord_T rec;
		if (!msc_filerec(slot - 2, &rec)) { memset(entry, 0, 32); return; } // out of range
		msgstore_file_name(rec, name);
		set_entry(entry, name, 0x01, (uint16_t)(4 + mdc + (slot - 2)), msgstore_record_content_len(rec));
	}
}

// ME/ subdirectory: 0 = ".", 1 = "..", 2 = the own item (if present).
static uint32_t msc_me_nentries(void) { return msgstore_own_present() ? 3 : 2; }

static void msc_me_entry(uint32_t idx, uint8_t entry[32], uint16_t mdc) {
	uint8_t name[11]; memset(name, ' ', 11);
	if (idx == 0) {
		name[0] = '.';
		set_entry(entry, name, 0x10, (uint16_t)(2 + mdc), 0);   // "."  -> self
	} else if (idx == 1) {
		name[0] = '.'; name[1] = '.';
		set_entry(entry, name, 0x10, 0, 0);                     // ".." -> root
	} else {
		msgstore_own_name(name);
		set_entry(entry, name, 0x20 /*writable*/, (uint16_t)(3 + mdc), msgstore_own_content_len());
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

// ME/ host-write capture. The host edits/uploads/renames/deletes the single own
// item in ME/. Buffer the ME/ subdirectory sector (which carries the file's
// name/size/presence) and any own-file content write, then commit to flash from
// usb_task() - never from the IRQ. me_first_lba = the ME/ subdir cluster's LBA.
static volatile uint32_t me_first_lba;
static uint8_t  me_subdir_buf[512];
static uint8_t  own_content_buf[256];
static volatile uint8_t  me_have_subdir, own_have_content, me_commit_pending;

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
			} else if (kind == DA_OWN || kind == DA_FILE) {
				const uint8_t *content; uint32_t clen;
				if (kind == DA_OWN) {
					content = msgstore_own_content(); clen = msgstore_own_content_len();
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
				me_first_lba = START_DATA + (uint32_t)msc_md_clusters() * 8u; // ME/ subdir cluster
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
			} else if (lba >= START_DATA) {           // own file content (reallocated cluster)
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < sizeof(own_content_buf)) own_content_buf[off] = data[i];
				}
				own_have_content = 1;
			}

			msc_bytes_remaining -= n;
			if (msc_bytes_remaining == 0) {
				if (me_have_subdir) me_commit_pending = 1;  // commit in usb_task (out of IRQ)
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

// Find the own item's 8.3 entry in a captured ME/ subdir sector.
// Returns 1 + fills name83/size if present, 0 if ME/ is empty (no file entry).
static int me_parse_own(const uint8_t *sec, uint8_t name83[11], uint32_t *size) {
	for (int e = 0; e < 16; e++) {              // 16 entries in a 512-byte sector
		const uint8_t *en = sec + e * 32;
		uint8_t c0 = en[0], attr = en[0x0B];
		if (c0 == 0x00) break;                  // end of directory
		if (c0 == 0xE5) continue;               // deleted
		if (attr == 0x0F) continue;             // long-filename entry
		if (attr & 0x18) continue;              // subdirectory (. / ..) or volume label
		memcpy(name83, en, 11);                 // first real file = the own item
		*size = en[0x1C] | (en[0x1D] << 8) | (en[0x1E] << 16) | ((uint32_t)en[0x1F] << 24);
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
		uint8_t name83[11]; uint32_t fsize = 0;
		if (me_parse_own(me_subdir_buf, name83, &fsize)) {
			PeerRecordType_T type = name83_is_txt(name83) ? RECORD_TYPE_TEXT : RECORD_TYPE_FILE;
			if (own_have_content) {              // edit/upload: use the written content
				uint16_t l = (fsize > SYNTHPASS_MAX_MSG_SIZE) ? SYNTHPASS_MAX_MSG_SIZE : (uint16_t)fsize;
				msgstore_own_set(type, name83, own_content_buf, l);
			} else {                             // rename: keep content, update name/type
				uint16_t clen = msgstore_own_content_len();
				uint8_t tmp[256];
				if (clen > sizeof(tmp)) clen = sizeof(tmp);
				memcpy(tmp, msgstore_own_content(), clen); // copy before own_set erases flash
				msgstore_own_set(type, name83, tmp, clen);
			}
		} else {
			msgstore_own_clear();                // ME/ emptied -> broadcast nothing
		}
		own_have_content = 0;
		me_have_subdir = 0;
	}

	// When the host has taken the previous MSC IN packet, send the next.
	if (msc_state == MSC_DATA_IN) {
		MSC_PrepareDataIn();
	}
}

void usb_reset(void) {
	USBFSReset();
}
