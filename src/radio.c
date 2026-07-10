/*
 * SynthPass radio + protocol over ch32fun's iSLER (2.4GHz BLE-advertising
 * frames, fixed access address / channel / MAC). Broadcasts our UID, replies to
 * peers' broadcasts with PROX (proximity + RSSI), and ramps the broadcast rate
 * as peers get closer. Debug output goes to the CDC link via printf (usb.c).
 */
#include "radio.h"
#include <stdint.h>
#include <stdio.h>
#include "ch32fun.h"
#include "ch5xxhw.h"
#include "iSLER.h"
#include "synthpass.h"
#include "board.h"
#include "lib_rand.h"
#include "msgstore.h"
#include "config.h"

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
	memset(&peers[oldest], 0, sizeof(peers[oldest]));
	peers[oldest].peer_uid = peer_uid;
	peers[oldest].calib_rssi = rssi;
	peers[oldest].last_seen = now;
	// next_prox=NOBOOP, resp.type=BROADCAST, our_*_data_acked=0 are all 0/zero values

	return oldest;
}

static uint8_t peer_add_response(SynthPass_PeerState_T *peer, SynthPass_MessageType_T type) {
	// Priority ladder for the single response slot:
	//   PROX                 -- lowest; pure ranging, regenerates every broadcast
	//   BOOP / UNBOOP        -- state-change signals; pre-empt a pending PROX so
	//                           the peer learns about the transition without
	//                           waiting for another broadcast round-trip
	//   *_DATA / *_DATA_ACK  -- highest; pre-empt anything
	uint8_t high_pri = (type == SYNTHPASS_PROX_DATA || type == SYNTHPASS_BOOP_DATA
	                 || type == SYNTHPASS_PROX_DATA_ACK || type == SYNTHPASS_BOOP_DATA_ACK);
	uint8_t state_change = (type == SYNTHPASS_BOOP || type == SYNTHPASS_UNBOOP);
	SynthPass_MessageType_T have = peer->resp.type;
	int ok = (have == SYNTHPASS_BROADCAST)
	      || high_pri
	      || (state_change && have == SYNTHPASS_PROX);
	if(!ok) {
		return 0;
	}
	SynthPass_QueuedResponse_T resp = {
		.send_at = funSysTick32() + (RESPONSE_DELAY_MS * DELAY_MS_TIME),
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
		// Pack our own record onto the wire mirroring the msgstore RECEIVED view:
		//   [peer_uid u32][record_type u8][payload...]
		// where payload is what msgstore_received_append() expects -- raw content
		// for TEXT, name83[11]+content for FILE. OWN always stores with the
		// 11-byte name prefix (it's how MSC exposes the file), so strip it for
		// TEXT before sending. PROX uses PROX.TXT; BOOP uses BOOP.TXT if present
		// (else falls back to PROX.TXT via msgstore_own_for_boop).
		SynthPassPeerRecord_T own = (resp_type == SYNTHPASS_BOOP_DATA)
		                            ? msgstore_own_for_boop()
		                            : msgstore_own(MSGSTORE_OWN_PROX);
		if(own.data_length < 11) {
			return; // empty / not present, nothing to send
		}
		uint16_t prefix = (own.record_type == RECORD_TYPE_TEXT) ? 11u : 0u;
		const uint8_t *src = own.data + prefix;
		uint16_t src_len = (uint16_t)(own.data_length - prefix);

		SynthPass_ProxData_T msg;
		msg.peer_uid = peer->peer_uid;
		msg.user_info[0] = (uint8_t) own.record_type;
		uint16_t max_payload = (uint16_t)(sizeof(msg.user_info) - 1);
		uint16_t copy = (src_len > max_payload) ? max_payload : src_len;
		memcpy(&msg.user_info[1], src, copy);
		uint16_t tx_len = (uint16_t)(sizeof(msg.peer_uid) + 1u + copy);
		uint8_t status = synthpass_tx(resp_type, (uint8_t*)&msg, (uint8_t)tx_len);
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
	uint32_t earliest = 0;
	int earliest_idx = -1;
	for(uint32_t i = 0; i < MAX_PEERS; ++i) {
		if(peers[i].peer_uid == 0) continue; // uninitialized, skip
		if(peers[i].resp.type != SYNTHPASS_BROADCAST) {
			// First candidate seeds `earliest`; later ones overtake it only if
			// strictly older (wrap-safe). Tracking the "found" state separately
			// avoids a UINT32_MAX sentinel that would break the signed-diff
			// comparison on small tick values.
			if(earliest_idx == -1 || (int32_t)(peers[i].resp.send_at - earliest) < 0) {
				earliest = peers[i].resp.send_at;
				earliest_idx = i;
			}
		}
	}

	if(earliest_idx == -1) return -1;
	uint32_t now = funSysTick32();
	return ((int32_t)(now - earliest) > 0) ? earliest_idx : -1;
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
	// Drop frames the hardware flagged as CRC-failed before trusting any of
	// their bytes. iSLERCRCOK()==0 means a confirmed CRC failure; -1 means
	// unimplemented on this target, in which case we fail open (unchanged
	// behavior) rather than dropping every packet.
	if(iSLERCRCOK() == 0) {
		return;
	}

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

					const Config_T *cfg = config_get();
					int boop_thr = BOOP_RSSI + cfg->boop_rssi_adjust;
					if(corrected_rssi > boop_thr && rxData->rx_rssi > boop_thr && peer_state->next_prox != BOOP) {
						printf("Booping...\r\n");
						peer_state->next_prox = BOOP;
						// Push the state change to the peer now (don't wait for
						// their next broadcast); BOOP can pre-empt a queued PROX.
						peer_add_response(peer_state, SYNTHPASS_BOOP);
					}

					// Queue PROX.TXT back to this peer unless config opted out
					// (DATA_TRIGGER_BOOP_ONLY skips the prox-time data send).
					if(cfg->data_trigger == DATA_TRIGGER_PROX_AND_BOOP
					   && !peer_state->our_prox_data_acked
					   && msgstore_own(MSGSTORE_OWN_PROX).data_length >= 11) {
						peer_add_response(peer_state, SYNTHPASS_PROX_DATA);
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

					// Receiving BOOP means the peer has already decided we're
					// booped; mirror their state so a marginal local RSSI gate
					// can't leave the two boards desynced. The mutual-RSSI gate
					// still controls *un*-boop -- it takes precedence here so
					// distance increases tear down both sides cleanly.
					int unboop_thr = UNBOOP_RSSI + config_get()->boop_rssi_adjust;
					if(corrected_rssi < unboop_thr && rxData->rx_rssi < unboop_thr) {
						printf("Un-booping...\r\n");
						peer_state->next_prox = UNBOOP;
						peer_add_response(peer_state, SYNTHPASS_UNBOOP);
					} else if(peer_state->next_prox != BOOP) {
						printf("Booping (peer-driven)...\r\n");
						peer_state->next_prox = BOOP;
					}

					// Queue BOOP.TXT (or PROX.TXT fallback) until acked.
					if(!peer_state->our_boop_data_acked && msgstore_own_for_boop().data_length >= 11) {
						peer_add_response(peer_state, SYNTHPASS_BOOP_DATA);
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
		case SYNTHPASS_PROX_DATA:
		case SYNTHPASS_BOOP_DATA:
			{
				// Peer is delivering their own message. The packed framing is
				// [peer_uid u32][record_type u8][msgstore-style payload] -- mirrors
				// the on-flash record view so the tail goes straight to append().
				SynthPass_ProxData_T *rxData = (SynthPass_ProxData_T*) frame->msg.data;
				int data_len = (int)frame->length - (int)SYNTHPASS_MAC_SIZE - (int)sizeof(SynthPass_Header_T);
				int min_len = (int)sizeof(rxData->peer_uid) + 1; // +1 for record_type
				if(data_len < min_len) {
					printf("*_DATA: short frame (%d)\r\n", data_len);
					break;
				}
				if(rxData->peer_uid != synthpass_uid) {
					printf("(not for me) %s\r\n", type == SYNTHPASS_PROX_DATA ? "PROX_DATA" : "BOOP_DATA");
					break;
				}
				PeerRecordType_T rec_type = (PeerRecordType_T) rxData->user_info[0];
				const uint8_t *payload = &rxData->user_info[1];
				uint16_t payload_len = (uint16_t)(data_len - min_len);

				if(rec_type == RECORD_TYPE_TEXT || rec_type == RECORD_TYPE_FILE) {
					if(!msgstore_received_has(peer_uid, rec_type, payload, payload_len)) {
						printf("Storing new record from peer");
						printf_uid(peer_uid);
						printf(" (type=%d, len=%u)\r\n", rec_type, payload_len);
						msgstore_received_append(peer_uid, rec_type, payload, payload_len);
					} else {
						printf("Duplicate *_DATA from peer");
						printf_uid(peer_uid);
						printf("\r\n");
					}
				} else {
					printf("*_DATA: bad record_type %d\r\n", rec_type);
				}

				// Always ack so the peer stops resending.
				SynthPass_MessageType_T ack = (type == SYNTHPASS_PROX_DATA) ?
				                              SYNTHPASS_PROX_DATA_ACK : SYNTHPASS_BOOP_DATA_ACK;
				peer_add_response(peer_state, ack);
			}
			break;
		case SYNTHPASS_PROX_DATA_ACK:
		case SYNTHPASS_BOOP_DATA_ACK:
			{
				SynthPass_ProxDataAck_T *rxAck = (SynthPass_ProxDataAck_T*) frame->msg.data;
				if(rxAck->peer_uid == synthpass_uid) {
					printf("%s from peer", type == SYNTHPASS_PROX_DATA_ACK ? "PROX_DATA_ACK" : "BOOP_DATA_ACK");
					printf_uid(peer_uid);
					printf("\r\n");
					if (type == SYNTHPASS_PROX_DATA_ACK) peer_state->our_prox_data_acked = 1;
					else                                  peer_state->our_boop_data_acked = 1;
				} else {
					printf("(not for me) DATA_ACK\r\n");
				}
			}
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
	memcpy((char*) tx_frame.mac, SYNTHPASS_MAC, SYNTHPASS_MAC_SIZE);

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

		// Drive the indicator outputs from the config (LED behavior is a knob;
		// QWIIC GPIO writes are gated on protocol_mode so a future I2C/serial
		// mode can take over those pins).
		int booped = is_any_peer_booped(peers);
		int active = booped || is_any_peer_active(peers);
		const Config_T *cfg = config_get();
		int led_on = (cfg->led_behavior == LED_BEHAVIOR_BOOP) ? booped
		           : (cfg->led_behavior == LED_BEHAVIOR_PROX) ? active
		           : 0;
		if(led_on) LED_ON(); else LED_OFF();
#ifdef HAVE_QWIIC_GPIO
		if(cfg->protocol_mode == PROTOCOL_GPIO) {
			funDigitalWrite(QWIIC_PROX_PIN, active ? FUN_HIGH : FUN_LOW);
			funDigitalWrite(QWIIC_BOOP_PIN, booped ? FUN_HIGH : FUN_LOW);
		}
#endif
		uint32_t broadcast_period_ticks = broadcast_random_ticks;
		if(booped)      broadcast_period_ticks += SYNTHPASS_BOOP_PERIOD      * DELAY_MS_TIME;
		else if(active) broadcast_period_ticks += SYNTHPASS_PROX_PERIOD      * DELAY_MS_TIME;
		else            broadcast_period_ticks += SYNTHPASS_BROADCAST_PERIOD * DELAY_MS_TIME;

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
