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

// Max message payload that fits in a single frame's data field (i.e. after the
// link-layer/advertising header). This bounds the user's own broadcast message.
#define SYNTHPASS_MAX_MSG_SIZE (SYNTHPASS_DATA_SIZE - sizeof(SynthPass_Header_T))


// BLE advertisements are sent on channels 37, 38, and 39.
// Arbitrarily choosing 37 so that synthpass devices don't need to channel hop to find each other.
#define SYNTHPASS_CHANNEL 37


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
typedef struct __attribute__((__packed__)) {
    uint32_t peer_uid;
    int8_t rx_rssi;
} SynthPass_Prox_T; // Prox/Boop/Unboop

typedef struct __attribute__((__packed__)) {
    uint32_t peer_uid;
    uint8_t user_info[237];
} SynthPass_ProxData_T; // ProxData/BoopData

typedef struct __attribute__((__packed__)) {
    uint32_t peer_uid;
} SynthPass_ProxDataAck_T; // ProxDataAck/BoopDataAck


// ---------- Runtime Peer Info -----------

#define MAX_PEERS 32
// responses sent at least this long after the message received (so that they have time to go back into RX mode).
#define RESPONSE_DELAY_MS 0

// queued response - to be sent back to the peer after a delay
typedef struct {
	uint32_t send_at; // systick timestamp to send the message at
	SynthPass_MessageType_T type; // 0x00/BROADCAST = nothing queued.
} SynthPass_QueuedResponse_T;

typedef enum {
	NOBOOP,
	BOOP,
	UNBOOP
} SynthPass_NextProx_T;

typedef struct {
	uint32_t peer_uid;
	int calib_rssi; // calibrated rssi, should match what two nanoch57x's would see at the same distance.
	SynthPass_NextProx_T next_prox; // whether we're booped, unbooped, or not booped. Affects what we'll respond with to the next broadcast.
	uint32_t last_seen; // milliseconds timestamp when this peer was last seen.
	SynthPass_QueuedResponse_T resp; // set to a message type/timestamp if we need to respond to this peer.
	uint8_t our_prox_data_acked; // 1 once this peer has ack'd our PROX_DATA; we stop sending PROX_DATA to them.
	uint8_t our_boop_data_acked; // 1 once this peer has ack'd our BOOP_DATA; we stop sending BOOP_DATA to them.
} SynthPass_PeerState_T;


// ---- Timing & Calibration parameter ----
// SYNTHPASS_REF_RSSI / SYNTHPASS_REF_RXRSSI are per-board calibration offsets;
// they live in board.h since each hardware variant has a different value.

#define BOOP_RSSI -30 				// Anything below this is considered boopworthy!
#define UNBOOP_RSSI -35 			// RSSI this low triggers unboop

#define SYNTHPASS_BROADCAST_PERIOD 1000 // (milliseconds) interval between sending broadcast packets without peers
#define SYNTHPASS_PROX_PERIOD 200       // (milliseconds) interval between sending broadcast packets with at least one peer responding with PROX messages.
#define SYNTHPASS_BOOP_PERIOD 100       // (milliseconds) interval between sending broadcast packets with at least one peer
#define SYNTHPASS_RANDOM_DELAY 20       // (milliseconds) random delay up to this time is added to each broadcast period to avoid repeated collisions.


#define SYNTHPASS_TIMEOUT 3000     // (milliseconds) If no messages received in this time, assume the boop ended and unboop/disconnect peer.
// ----------------------------------------

#endif