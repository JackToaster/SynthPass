/*
 * Composite USB device for SynthPass.
 *   - CDC ACM  (EP1/2/3) : debug serial -- printf out via _write(), input via
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
	'F', 'A', 'T', '1', '6', ' ', ' ', ' ',         // FS Type
	[510] = 0x55,
	[511] = 0xAA
};

// The volume presents one file per record: file 0 = own message ("OWN.TXT"),
// files 1..N = received messages ("00001.TXT", "00002.TXT", ...). File f lives
// in cluster (FILE_CLUSTER + f); each occupies a single cluster.
static SynthPassPeerRecord_T msc_file(uint32_t f) {
	if (f == 0) return msgstore_own();
	return msgstore_received(f - 1);
}

// 8.3 name (11 bytes, space-padded) for file f: "OWN     TXT" / "00001   TXT".
static void msc_file_name(uint32_t f, uint8_t name[11]) {
	memset(name, ' ', 11);
	if (f == 0) {
		memcpy(name, "OWN", 3);
	} else {
		uint32_t n = f; // received[f-1] -> file number f (1 -> "00001")
		for (int i = 4; i >= 0; i--) { name[i] = '0' + (n % 10); n /= 10; }
	}
	memcpy(name + 8, "TXT", 3);
}

// Build the 32-byte FAT root-directory entry for file f.
static void msc_dir_entry(uint32_t f, uint8_t entry[32]) {
	memset(entry, 0, 32);
	msc_file_name(f, entry);
	entry[0x0B] = 0x20;                      // attribute: archive
	entry[0x16] = 0x21; entry[0x18] = 0x21;  // write time/date (arbitrary, non-zero)
	uint16_t cluster = (uint16_t)(FILE_CLUSTER + f);
	entry[0x1A] = cluster & 0xFF;
	entry[0x1B] = cluster >> 8;
	uint32_t size = msc_file(f).data_length;
	entry[0x1C] = size & 0xFF;
	entry[0x1D] = (size >> 8) & 0xFF;
	entry[0x1E] = (size >> 16) & 0xFF;
	entry[0x1F] = (size >> 24) & 0xFF;
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

// OWN.TXT host-write capture. Buffer the bytes the host writes to OWN.TXT's data
// sector; the length is the content up to the zero-padded tail (capped at one
// SynthPass message payload). The flash write is committed from usb_task() --
// never from the USB IRQ, since flash erase/write blocks interrupts for ms.
static uint8_t  own_write_buf[SYNTHPASS_MAX_MSG_SIZE];
static volatile uint16_t own_write_len;
static volatile uint8_t  own_have_data, own_commit_pending;

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
		uint32_t nfiles = 1 + msgstore_received_count();   // own + received

		// --- Boot sector ---
		if (current_lba == 0) {
			memcpy(msc_response_buffer, &BootSector[boff], len_to_send);
		}
		// --- FAT tables: cluster 0 = F8FF, cluster 1 = FFFF, and each file's
		//     single cluster (2..1+nfiles) marked EOF (FFFF); rest free (0). ---
		else if (current_lba == START_FAT1 || current_lba == START_FAT2) {
			uint32_t fat_used = 2u * (2u + nfiles);
			for (uint32_t i = 0; i < len_to_send; i++) {
				uint32_t pos = boff + i;
				if (pos == 0) msc_response_buffer[i] = 0xF8;
				else if (pos < fat_used) msc_response_buffer[i] = 0xFF;
			}
		}
		// --- Root directory: one 32-byte entry per file (first root sector) ---
		else if (current_lba == START_ROOT) {
			for (uint32_t e = 0; e * 32 < len_to_send; e++) {
				uint32_t f = boff / 32 + e;
				if (f < nfiles) {
					uint8_t entry[32];
					msc_dir_entry(f, entry);
					uint32_t n = (len_to_send - e * 32 < 32) ? (len_to_send - e * 32) : 32;
					memcpy(msc_response_buffer + e * 32, entry, n);
				}
			}
		}
		// --- Data area: cluster (2+f) holds file f's data in its first sector ---
		else if (current_lba >= START_DATA) {
			uint32_t f = (current_lba - START_DATA) / 8;       // cluster (2+f) -> file f
			uint32_t sector_in_cluster = (current_lba - START_DATA) % 8;
			if (f < nfiles && sector_in_cluster == 0) {
				SynthPassPeerRecord_T rec = msc_file(f);
				for (uint32_t i = 0; i < len_to_send; i++) {
					uint32_t off = boff + i;
					if (off < rec.data_length) msc_response_buffer[i] = rec.data[off];
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
		msc_response_buffer[2] = 0x00; // device-specific param: WP=0 (writable, for OWN.TXT)
		len_to_send = 4;
		src_ptr = msc_response_buffer;
		is_short_transfer = 1;
		break;

	case 0x5A: // MODE SENSE 10
		memset(msc_response_buffer, 0, 8);
		msc_response_buffer[1] = 6;    // mode data length (low byte)
		msc_response_buffer[3] = 0x00; // device-specific param: WP=0 (writable, for OWN.TXT)
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

		// --- 2. DATA OUT State: capture OWN.TXT, discard everything else ---
		// OWN.TXT is file 0: its content lives in the first data sector
		// (START_DATA) and its size in root-directory entry 0 (START_ROOT).
		// Buffer both; other sectors (FAT / other files) are drained and dropped
		// -- they revert to the synthesized view on remount. The flash write is
		// deferred to usb_task() (out of the IRQ).
		else if (msc_state == MSC_DATA_OUT) {
			uint32_t written = cbw.DataTransferLength - msc_bytes_remaining; // bytes before this packet
			uint32_t lba  = msc_write_lba + written / MSC_BLOCK_SIZE;
			uint32_t boff = written % MSC_BLOCK_SIZE;
			uint32_t n = (len < msc_bytes_remaining) ? len : msc_bytes_remaining;

			// OWN.TXT is the only host-writable file, so any data-area write holds
			// its content. The host may reallocate OWN.TXT to a different cluster
			// than our synthesized layout (vfat allocates from a "next free" hint),
			// so accept any cluster's first sector, not just START_DATA.
			if (lba >= START_DATA && ((lba - START_DATA) % 8) == 0) {
				for (uint32_t i = 0; i < n; i++) {
					uint32_t off = boff + i;
					if (off < sizeof(own_write_buf)) own_write_buf[off] = data[i];
				}
				own_have_data = 1;
			}

			msc_bytes_remaining -= n;
			if (msc_bytes_remaining == 0) {
				if (own_have_data) {
					// Length = content up to the last non-zero byte (the host
					// zero-pads the unused tail of the sector); capped already by
					// the buffer size = SYNTHPASS_MAX_MSG_SIZE.
					uint16_t l = sizeof(own_write_buf);
					while (l > 0 && own_write_buf[l - 1] == 0) l--;
					own_write_len = l;
					own_commit_pending = 1;            // flash write deferred to usb_task()
					own_have_data = 0;
				}
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

void usb_task(void) {
	// Persist a host-written OWN.TXT (flash erase/write must run here, not in the
	// USB IRQ). peer_id 0 -- the store version marker lives in the README record.
	if (own_commit_pending) {
		own_commit_pending = 0;
		msgstore_own_set(0, own_write_buf, own_write_len);
	}

	// When the host has taken the previous MSC IN packet, send the next.
	if (msc_state == MSC_DATA_IN) {
		MSC_PrepareDataIn();
	}
}

void usb_reset(void) {
	USBFSReset();
}
