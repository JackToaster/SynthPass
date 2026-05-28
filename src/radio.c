/*
 * SynthPass radio + protocol over ch32fun's iSLER (2.4GHz BLE-advertising
 * frames, fixed access address / channel / MAC). Broadcasts our UID, replies to
 * peers' broadcasts with PROX (proximity + RSSI), and ramps the broadcast rate
 * as peers get closer. Debug output goes to the CDC link via printf (usb.c).
 */
#include "radio.h"
#include <stdio.h>
#include <string.h>
#include "ch32fun.h"
#include "iSLER.h"
#include "synthpass.h"

__attribute__((aligned(4))) static SynthPass_Frame_T tx_frame = {};
static uint32_t synthpass_uid;
static uint32_t last_broadcast_tick;
static uint32_t broadcast_random_ticks;

// TODO replace with two timers (last_prox, last_booped)
static Synthpass_BroadcastPeriod_T period = BROADCAST_PERIOD_NORMAL;

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
		printf("beep!\n");
	} else {
		printf("sad beep :(\n");
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

static void incoming_frame_handler(void) {
	// The chip stores the incoming frame in LLE_BUF, defined in extralibs/iSLER.h
	volatile SynthPass_Frame_T *frame = (volatile SynthPass_Frame_T*)LLE_BUF;

	// check if the RX'd frame is a synthpass frame (PDU and MAC match)
	if(!validate_synthpass_frame(frame)) {
		return;
	}

	int rssi = iSLERRSSI();
	int corrected_rssi = rssi - frame->msg.hdr.ref_rssi - SYNTHPASS_REF_RXRSSI;

	SynthPass_MessageType_T type = frame->msg.hdr.msg_type;

	switch(type) {
		case SYNTHPASS_BROADCAST:
			{
				// received broadcast data from another SynthPass, reply with PROX frame
				printf("BROADCAST peer uid");
				printf_uid(frame->msg.hdr.sender_uid);
				printf("\n");

				SynthPass_Prox_T msg = {
					.peer_uid=frame->msg.hdr.sender_uid,
					.rx_rssi=corrected_rssi
				};

				// Wait for the peer to transition back into RX after its broadcast
				// before replying, otherwise the PROX arrives while it can't receive.
				Delay_Ms(10);

				uint8_t status = synthpass_tx(SYNTHPASS_PROX, (uint8_t*)&msg, sizeof(msg));
				(void)status;

				// switch to faster message rate (period is the enum, not the ms value)
				if (period == BROADCAST_PERIOD_NORMAL) period = BROADCAST_PERIOD_PROX;
				synthpass_rx();
			}
			break;
		case SYNTHPASS_PROX:
			{
				// Received a response
				SynthPass_Prox_T *rxData = (SynthPass_Prox_T *) frame->msg.data;
				if(rxData->peer_uid == synthpass_uid) {
					printf("PROX peer uid");
					printf_uid(frame->msg.hdr.sender_uid);
					printf(" rx_rssi=%d\n", rxData->rx_rssi);
				} else {
					printf("(not for me) PROX\n");
				}
			}
			break;
		default:
			printf("Unrecognized type %d\n", type);
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
	printf("\n");

	// set last broadcast time to now
	last_broadcast_tick = funSysTick32();
	broadcast_random_ticks = 0;

	// first broadcast
	synthpass_broadcast();
	// start listening for frames
	synthpass_rx();
}

void radio_init(void) {
	iSLERInit(LL_TX_POWER_0_DBM);
	synthpass_init();
}

void radio_task(void) {
	// Handle a received frame
	if(rx_ready) {
		incoming_frame_handler();
		synthpass_rx();
	}

	// Periodic broadcast.
	uint32_t broadcast_period_ticks = broadcast_random_ticks;
	if(period == BROADCAST_PERIOD_NORMAL) { broadcast_period_ticks += SYNTHPASS_BROADCAST_PERIOD * DELAY_MS_TIME; }
	else if(period == BROADCAST_PERIOD_PROX) { broadcast_period_ticks += SYNTHPASS_PROX_PERIOD * DELAY_MS_TIME; }
	else /* BROADCAST_PERIOD_BOOP */ { broadcast_period_ticks += SYNTHPASS_BOOP_PERIOD * DELAY_MS_TIME; }

	if(funSysTick32() - last_broadcast_tick > broadcast_period_ticks) {
		synthpass_broadcast();
		synthpass_rx();
		broadcast_random_ticks = 0; // todo randomize
	}
}
