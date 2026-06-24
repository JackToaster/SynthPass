/*
 * SynthPass radio + protocol over ch32fun's iSLER (2.4GHz BLE-advertising
 * frames, fixed access address / channel / MAC). Broadcasts our UID, replies to
 * peers' broadcasts with PROX (proximity + RSSI), and ramps the broadcast rate
 * as peers get closer. Debug output goes to the CDC link via printf (usb.c).
 */
#include "radio.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ch32fun.h"
#include "ch5xxhw.h"
#include "iSLER.h"
#include "msgstore.h"
#include "synthpass.h"
#include "board.h"
#include "lib_rand.h"

__attribute__((aligned(4))) static SynthPass_Frame_T tx_frame = {};
static uint32_t synthpass_uid;
static uint32_t last_broadcast_tick;
static uint32_t broadcast_random_ticks;

static void synthpass_rx(void) {
	iSLERRX(ACCESS_ADDRESS, SYNTHPASS_CHANNEL, SYNTHPASS_PHY_MODE);
}

static uint8_t synthpass_tx(SynthPass_MessageType_T type, uint8_t* data, uint8_t data_length) {
	uint32_t len = data_length + sizeof(SynthPass_Header_T) + SYNTHPASS_MAC_SIZE;
	if(len > 255) {
		return 1;
	}
	tx_frame.length = len;

	tx_frame.msg.hdr.ad_len = data_length + sizeof(SynthPass_Header_T) - 1; // length of message+header, minus the ad_len byte itself

	tx_frame.msg.hdr.msg_type = type;
	memcpy(tx_frame.msg.data, data, data_length);

	uint32_t isler_frame_length = len + 2;

	iSLERTX(ACCESS_ADDRESS, (uint8_t*)&tx_frame, isler_frame_length, SYNTHPASS_CHANNEL, SYNTHPASS_PHY_MODE);

	return 0;
}

static uint8_t synthpass_broadcast(void) {

	uint8_t status = synthpass_tx(SYNTHPASS_BROADCAST, 0, 0);
	if(status == 0) {
		// printf("beep!\r\n");
	} else {
		printf("sad beep :( (broadcast failed?!)\r\n");
	}
	last_broadcast_tick = funSysTick32();
	return status;
}

static void printf_uid(uint32_t uid) {
	for(int i = 0; i < 4; ++i) {
		printf(":%02x", (unsigned)((uid >> (8 * i)) & 0xFF));
	}
}

static uint8_t validate_synthpass_frame(volatile SynthPass_Frame_T *frame) {
	return (
		frame->pdu == SYNTHPASS_PDU
		&& (strncmp((const char*)(frame->mac), SYNTHPASS_MAC, SYNTHPASS_MAC_SIZE) == 0)
	) ? 1 : 0;
}

// find existing peer, -1 on not found
static int find_peer(uint32_t peer_uid, SynthPass_PeerState_T *peers) {
	for(int i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid == peer_uid) {
			return i;
		}
	}
	return -1;
}

// add new peer to the list and return its index
static int insert_peer(uint32_t peer_uid, SynthPass_PeerState_T *peers, int rssi) {
	uint32_t now = funSysTick32();
	int oldest = 0;
	uint32_t oldest_age = 0;
	// find oldest entry, or first empty entry
	for(int i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid == 0) { // already uninitialized, just return this entry

			// printf("uninit %d\r\n", i);
			oldest = i;
			break;
		}
		uint32_t age = now - peers[i].last_seen;
		if(age > oldest_age) {
			oldest = i;
			oldest_age = age;
		}
	}

	// printf("oldest %d\r\n", oldest);
	// clear the oldest entry
	peers[oldest].peer_uid = peer_uid;
	peers[oldest].calib_rssi = rssi;
	peers[oldest].next_prox = NOBOOP;
	peers[oldest].last_seen = now;
	peers[oldest].resp.type = SYNTHPASS_BROADCAST; // no response

	return oldest;
}

static uint8_t peer_add_response(SynthPass_PeerState_T *peer, SynthPass_MessageType_T type) {
	uint32_t now = funSysTick32();
	// don't clobber already queue'd responses
	if(peer->resp.type != 0) {
		return 0;
	}
	SynthPass_QueuedResponse_T resp = {
		.send_at = now + (RESPONSE_DELAY_MS * DELAY_MS_TIME),
		.type = type
	};
	peer->resp = resp;
	return 1;
}

static void peer_send_response(SynthPass_PeerState_T *peer) {
	SynthPass_MessageType_T resp_type = peer->resp.type;

	// clear so we don't rebroadcast
	peer->resp.type = SYNTHPASS_BROADCAST;
	
	if(resp_type == SYNTHPASS_PROX || resp_type == SYNTHPASS_BOOP || resp_type == SYNTHPASS_UNBOOP) { // "Prox-like" message
		// printf("Send prox %d\r\n", resp_type);
		SynthPass_Prox_T msg = {
			.peer_uid=peer->peer_uid,
			.rx_rssi=peer->calib_rssi
		};

		uint8_t status = synthpass_tx(resp_type, (uint8_t*)&msg, sizeof(msg));
		(void)status;
	} else if(resp_type == SYNTHPASS_BOOP_DATA || resp_type == SYNTHPASS_PROX_DATA) { // Data message
		printf("Send data\r\n");
		// TODO correct message type/data
		SynthPass_Prox_T msg = {
			.peer_uid=peer->peer_uid,
			.rx_rssi=peer->calib_rssi
		};

		uint8_t status = synthpass_tx(resp_type, (uint8_t*)&msg, sizeof(msg));
		(void)status;
	} else if(resp_type == SYNTHPASS_BOOP_DATA_ACK || resp_type == SYNTHPASS_PROX_DATA_ACK) { // ACK message
		printf("Send data ack\r\n");
		SynthPass_ProxDataAck_T msg = {
			.peer_uid=peer->peer_uid,
		};
		uint8_t status = synthpass_tx(resp_type, (uint8_t*)&msg, sizeof(msg));
		(void)status;
	} else { // unknown/bad type, skip
		return;
	}
	synthpass_rx();
}

uint8_t is_any_peer_booped(SynthPass_PeerState_T *peers) {
	for(uint32_t i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid == 0) continue; // uninitialized, skip
		if(peers[i].next_prox == BOOP) return 1;
	}
	return 0;
}

static uint8_t is_any_peer_active(SynthPass_PeerState_T *peers) {
	for(uint32_t i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid != 0) return 1;
	}
	return 0;
}

static int next_queued_peer_idx(SynthPass_PeerState_T *peers) {
	uint32_t earliest = UINT32_MAX;
	int earliest_idx = -1;
	for(uint32_t i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid == 0) continue; // uninitialized, skip
		if(peers[i].resp.type != SYNTHPASS_BROADCAST) {
			if((int32_t)(peers[i].resp.send_at - earliest) < 0) {
				earliest = peers[i].resp.send_at;
				earliest_idx = i;
			}
		}
	}

	// check if the earliest is ready to transmit, if so send it
	uint32_t now = funSysTick32();
	if((int32_t)(now - earliest) > 0) {
		return earliest_idx;
	} else {
		return -1;
	}
}

// loop through peers and timeout any olds
static void sync_peer_states(SynthPass_PeerState_T *peers) {
	uint32_t now = funSysTick32();
	for(uint32_t i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid == 0) { // already uninitialized, skip
			continue;
		}
		uint32_t age = now - peers[i].last_seen;
		if(age / DELAY_MS_TIME > SYNTHPASS_TIMEOUT) {
			printf("Peer timed out: ");
			printf_uid(peers[i].peer_uid);
			printf("\r\n");
			peers[i].next_prox = NOBOOP; // unboop if we haven't heard from them
			peers[i].peer_uid = 0; // erase peer
		}
	}
}

static void incoming_frame_handler(SynthPass_PeerState_T *peers) {
	// The chip stores the incoming frame in LLE_BUF, defined in extralibs/iSLER.h
	volatile SynthPass_Frame_T *frame = (volatile SynthPass_Frame_T*)LLE_BUF;

	// check if the RX'd frame is a synthpass frame (PDU and MAC match)
	if(!validate_synthpass_frame(frame)) {
		return;
	}

	int rssi = iSLERRSSI();
	int corrected_rssi = rssi - frame->msg.hdr.ref_rssi - SYNTHPASS_REF_RXRSSI;

	SynthPass_MessageType_T type = frame->msg.hdr.msg_type;
	uint32_t peer_uid = frame->msg.hdr.sender_uid;

	// check for malformed packet (uid = 0)
	if(peer_uid == 0) return;

	int peer_idx = find_peer(peer_uid, peers);

	// new peer - see if this is a broadcast. If not, ignore and return.
	if(peer_idx == -1) {
		if(type == SYNTHPASS_BROADCAST) {
			// broadcast from new peer! Add to the list.
			printf("New peer! ");
			printf_uid(peer_uid);
			printf("\r\n");
			peer_idx = insert_peer(peer_uid, peers, corrected_rssi);
			peer_add_response(&(peers[peer_idx]), SYNTHPASS_PROX);
		} else {
			return;
		}
	}
	SynthPass_PeerState_T *peer_state = &(peers[peer_idx]);


	uint32_t now = funSysTick32();

	peer_state->calib_rssi = corrected_rssi;
	peer_state->last_seen = now;

	switch(type) {
		case SYNTHPASS_BROADCAST:
			{
				// received broadcast data from another SynthPass, reply with PROX frame
				// printf("BROADCAST peer uid");
				// printf_uid(frame->msg.hdr.sender_uid);
				printf("\r\n");
				
				// printf("broadcast resp type %d\r\n", peer_state->next_prox);
				if(peer_state->next_prox == BOOP) {
					peer_add_response(peer_state, SYNTHPASS_BOOP);
				} else if(peer_state->next_prox == NOBOOP) {
					peer_add_response(peer_state, SYNTHPASS_PROX);
				} else if(peer_state->next_prox == UNBOOP) {
					uint8_t success = peer_add_response(peer_state, SYNTHPASS_UNBOOP);
					if(success) peer_state->next_prox = NOBOOP;
				}
			}
			break;
		case SYNTHPASS_PROX:
			{
				// Received a response
				SynthPass_Prox_T *rxData = (SynthPass_Prox_T *) frame->msg.data;

				if(rxData->peer_uid == synthpass_uid) {
					printf("PROX peer uid");
					printf_uid(frame->msg.hdr.sender_uid);
					printf(" rssi=%d rx_rssi=%d\r\n", corrected_rssi, rxData->rx_rssi);

					if(corrected_rssi > BOOP_RSSI && rxData->rx_rssi > BOOP_RSSI && peer_state->next_prox != BOOP) {
						printf("Booping...\r\n");
						peer_state->next_prox = BOOP; // set boop, we will send the boop packet next time.
						// TODO Queue boop data packet if not already sent
					}

				} else {
					printf("(not for me) PROX\r\n");
				}
			}
			break;
		case SYNTHPASS_BOOP:
			{
				// Received a response
				SynthPass_Prox_T *rxData = (SynthPass_Prox_T *) frame->msg.data;
				if(rxData->peer_uid == synthpass_uid) {
					printf("BOOP peer uid");
					printf_uid(frame->msg.hdr.sender_uid);
					printf(" rssi=%d rx_rssi=%d\r\n", corrected_rssi, rxData->rx_rssi);

					if(corrected_rssi < UNBOOP_RSSI && rxData->rx_rssi < UNBOOP_RSSI) {
						printf("Un-booping...\r\n");
						peer_state->next_prox = UNBOOP; // unset boop, we will send the prox packet next time
					}
				} else {
					printf("(not for me) BOOP\r\n");
				}
			}
			break;
		case SYNTHPASS_UNBOOP:
			{
				// Received a response
				SynthPass_Prox_T *rxData = (SynthPass_Prox_T *) frame->msg.data;
				if(rxData->peer_uid == synthpass_uid) {
					printf("UNBOOP peer uid");
					printf_uid(frame->msg.hdr.sender_uid);
					printf(" rssi=%d rx_rssi=%d\r\n", corrected_rssi, rxData->rx_rssi);

					peer_state->next_prox = NOBOOP; // unset boop
				} else {
					printf("(not for me) UNBOOP\r\n");
				}
			}
			break;
		case SYNTHPASS_BOOP_DATA:
			{
				// Received a response
				SynthPass_ProxData_T *rxData = (SynthPass_ProxData_T *) frame->msg.data;

				if(rxData->peer_uid == synthpass_uid) {
					printf("BOOP-DATA peer uid");
					printf_uid(frame->msg.hdr.sender_uid);
					
					uint32_t data_len = rxData->data_length; // i see what you're thinking

					if(data_len > sizeof(rxData->user_info)) data_len = sizeof(rxData->user_info); // do not the buffer overflow

					printf(" size %d\r\n", data_len);

					// todo need to dedup records?
					msgstore_received_append(peer_uid, rxData->record_type, rxData->user_info, data_len);

				} else {
					printf("(not for me) boopdata\r\n");
				}
			}
			break;
		case SYNTHPASS_PROX_DATA:
			break;
		default:
			printf("Unrecognized type %d\r\n", type);
			break;
	}
}

static void synthpass_init(void) {
	// Get our UID, based on the chip's built-in ID
	uint64_t chip_uid = *(uint64_t*)(0x3F018);
	synthpass_uid = (chip_uid >> 32) ^ (chip_uid & 0xFFFFFFFF);

	tx_frame.pdu = 0x02;
	tx_frame.length = SYNTHPASS_MAC_SIZE;
	strncpy((char*) tx_frame.mac, SYNTHPASS_MAC, SYNTHPASS_MAC_SIZE);

	tx_frame.msg.hdr.ad_type = 0xFF; // "Manufacturer specific data"

	tx_frame.msg.hdr.ref_rssi = SYNTHPASS_REF_RSSI;
	tx_frame.msg.hdr.sender_uid = synthpass_uid;

	printf("Synthpass init! UID");
	printf_uid(synthpass_uid);
	printf("\r\n");

	// set last broadcast time to now
	last_broadcast_tick = funSysTick32();
	broadcast_random_ticks = 0;

	// first broadcast
	synthpass_broadcast();
	// start listening for frames
	synthpass_rx();
}

static uint32_t next_tx_window;

void radio_init(void) {
	// we can transmit right away
	next_tx_window = funSysTick32();
	
	iSLERInit(LL_TX_POWER_0_DBM);
	synthpass_init();
}


void radio_task(SynthPass_PeerState_T *peers) {
	// Handle a received frame
	while(rx_ready) {
		incoming_frame_handler(peers);
		synthpass_rx();
	}


	// check if we can TX
	uint32_t now = funSysTick32();
	if((int32_t)(now - next_tx_window) > 0) {
		// set next tx window
		next_tx_window = now + (DELAY_MS_TIME * RESPONSE_DELAY_MS);

		// sync states
		sync_peer_states(peers);

		// calculate broadcast period
		uint32_t broadcast_period_ticks = broadcast_random_ticks;
		if(is_any_peer_booped(peers)) {
			LED_ON();
			broadcast_period_ticks += SYNTHPASS_BOOP_PERIOD * DELAY_MS_TIME;
		} else if(is_any_peer_active(peers)) {
			LED_OFF();
			broadcast_period_ticks += SYNTHPASS_PROX_PERIOD * DELAY_MS_TIME;
		} else {
			LED_OFF();
			broadcast_period_ticks += SYNTHPASS_BROADCAST_PERIOD * DELAY_MS_TIME;
		}

		// broadcast takes precedence
		if((int32_t)(now - last_broadcast_tick) > broadcast_period_ticks) {
			synthpass_broadcast();
			synthpass_rx();
			broadcast_random_ticks = rand() % (DELAY_MS_TIME * SYNTHPASS_RANDOM_DELAY); // randomize next broadcast period. Prevents repeated collisions.
			return;
		}

		int next_queued = next_queued_peer_idx(peers);
		if(next_queued == -1) return; // nothing queued
		peer_send_response(&(peers[next_queued]));
	}
}
