#ifndef SYNTHPASS_H
#define SYNTHPASS_H

#include <stdint.h>


// ---- Link layer details ----
// the "BED6" address for BLE advertisements
// Normally an access address for BLE communication is unique per connection, but since we're only sending
// and receiving advertising packets we'll just use this fixed/pre-defined address for all communication.
#define ACCESS_ADDRESS 0x8E89BED6
#define SYNTHPASS_PHY_MODE       PHY_1M

#define SYNTHPASS_PDU 0x02
#define SYNTHPASS_MAC (":3:3:3")
#define SYNTHPASS_MAC_SIZE 6
#define SYNTHPASS_DATA_SIZE (255 - SYNTHPASS_MAC_SIZE)


// BLE advertisements are sent on channels 37, 38, and 39.
// Arbitrarily choosing 37 so that synthpass devices don't need to channel hop to find each other.
#define SYNTHPASS_CHANNEL 37


// ----------------------------



// ---- SynthPass protocol definitions ----
typedef struct __attribute__((__packed__)) {
    uint8_t ad_len;     // advertising data unit length, counts remainder of the packet (not including the ad_len byte itself)
    uint8_t ad_type;    // advertising data type, use 0xFF ("manufacturer specific data")
	uint32_t sender_uid;
	uint8_t msg_type;
	int8_t ref_rssi; // RSSI as received by a NanoCH57x receiver at 1m distance. Used for calibrating RSSI between different synthpass hardware.
} SynthPass_Header_T;

typedef struct __attribute__((__packed__)) {
	SynthPass_Header_T hdr;
	uint8_t data[SYNTHPASS_DATA_SIZE - sizeof(SynthPass_Header_T)];
} SynthPass_Message_T;

typedef struct __attribute__((__packed__)) {
    // ---- Link-layer header ----
	uint8_t pdu; // == 0x02 for non-connectable non-directed advertisement
	uint8_t length; // length of remaining data (not including the PDU/length bytes)
    // ---------------------------

	uint8_t mac[SYNTHPASS_MAC_SIZE]; // == SYNTHPASS_MAC

	SynthPass_Message_T msg; // Remainder of packet data
} SynthPass_Frame_T;



typedef enum {
	SYNTHPASS_BROADCAST = 0x00, 	// Broadcast own UID to anyone who might be listening, no data. Period of broadcasts is defined below, with randomized time intervals to avoid repeated collisions.
	
	SYNTHPASS_PROX = 0x01, 			// Proximitiy ranging message - Peer UID & (calibrated) RSSI of received data. Sent in response to all received broadcast messages.
	SYNTHPASS_PROX_DATA = 0x11, 	// Proximity data message - "optional data" sent in response to broadcast message. (Option to split over multiple frames? Or is that too much data/overhead?).
									// This should be sent in response to all received PROX packets until it receives a PROX_DATA_ACK packet in response.
	SYNTHPASS_PROX_DATA_ACK = 0x21, // ACK of rx'd prox data - sender stores UID and stops sending them prox data upon receipt.
	
	SYNTHPASS_BOOP = 0x02, 			// Request peer to enter boop mode. Either peer can send one when RSSI ranging detects a boop. The other peer should respond with their own boop start message.
									// Boop start messages are identical to PROX messsages.

	SYNTHPASS_BOOP_DATA = 0x12,		// Same as SYNTHPASS_PROX_DATA, but for a boop.
	SYNTHPASS_BOOP_DATA_ACK = 0x22,	// Same as SYNTHPASS_PROX_DATA_ACK, but for boop_data.

	SYNTHPASS_UNBOOP = 0x03,		// Request peer to exit boop mode. Sent when RSSI ranging detects boop ending. If this is not received the boop will timeout automatically after SYNTHPASS_TIMEOUT.
									// Unboop messages are identical to PROX messages.
} SynthPass_MessageType_T;

// Message data structs
typedef struct {
    uint32_t peer_uid;
    int8_t rx_rssi;
} SynthPass_Prox_T; // Prox/Boop/Unboop

typedef struct {
    uint32_t peer_id;
    uint8_t user_info[237];
} SynthPass_ProxData_T; // ProxData/BoopData

typedef struct {
    uint32_t peer_id;
} SynthPass_ProxDataAck_T; // ProxDataAck/BoopDataAck

// ----------------------------------------


// ---- Timing & Calibration parameter ----
#define SYNTHPASS_REF_RSSI   0 		// RSSI as received by a NanoCH57x at 1m distance. Used for calibrating RSSI between different synthpass hardware.
#define SYNTHPASS_REF_RXRSSI 0		// RSSI of received frames sent by a NanoCH57x at 1m distance. This is used to calibrate out differences in RX sensitivity between different synthpass hardware.

#define SYNTHPASS_BROADCAST_PERIOD 1000 // (milliseconds) interval between sending broadcast packets without peers
#define SYNTHPASS_PROX_PERIOD 200       // (milliseconds) interval between sending broadcast packets with at least one peer responding with PROX messages.
#define SYNTHPASS_BOOP_PERIOD 20        // (milliseconds) interval between sending broadcast packets with at least one peer
#define SYNTHPASS_RANDOM_DELAY 20       // (milliseconds) random delay up to this time is added to each broadcast period to avoid repeated collisions.

typedef enum {
    BROADCAST_PERIOD_NORMAL,
    BROADCAST_PERIOD_PROX,
    BROADCAST_PERIOD_BOOP
} Synthpass_BroadcastPeriod_T;

#define SYNTHPASS_TIMEOUT 3000     // (milliseconds) If no BOOP or UNBOOP messages received in this time, assume the boop ended and unboop.
// ----------------------------------------

#endif