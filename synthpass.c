/* 
 * Minimal demo of iSLER with transmit and receive, on configurable PHY (1M, 2M, S2 or S8 if supported by the mcu)
 * It listens for advertisements from other BLE devices, and when one is detected it
 * changes it's own "Complete Local Name" to RX:XX where XX is the first byte of the detected BLE device's MAC.
 * The RX process happens on channel 37 AccessAddress 0x8E89BED6, which is defined in extralibs/iSLER.h.
 * When a new frame is received, the callback "incoming_frame_handler()" is called to process it.
 */
#include "ch32fun.h"
#include "ch5xxhw.h"
#include "iSLER.h"
#include <stdio.h>
#include <string.h>

#include "synthpass.h"

#define LED PA11


__attribute__((aligned(4))) SynthPass_Frame_T tx_frame = {};
uint32_t synthpass_uid;
uint32_t last_broadcast_tick;
uint32_t broadcast_random_ticks;

// TODO replace with two timers (last_prox, last_booped)
Synthpass_BroadcastPeriod_T period = BROADCAST_PERIOD_NORMAL;

void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		funDigitalWrite( LED, FUN_LOW ); // Turn on LED
		Delay_Ms(33);
		funDigitalWrite( LED, FUN_HIGH ); // Turn off LED
		if(i) Delay_Ms(33);
	}
}

void synthpass_rx() {
	iSLERRX(ACCESS_ADDRESS, SYNTHPASS_CHANNEL, SYNTHPASS_PHY_MODE);
}

uint8_t synthpass_tx(SynthPass_MessageType_T type, uint8_t* data, uint8_t data_length) {
	uint32_t len = data_length + sizeof(SynthPass_Header_T) + SYNTHPASS_MAC_SIZE;
	if(len > 255) {
		return 1;
	}
	tx_frame.length = len;

	tx_frame.msg.hdr.ad_len = data_length + sizeof(SynthPass_Header_T) - 1; // length of message+header, minus the ad_len byte itself
	
	tx_frame.msg.hdr.msg_type = type;
	memcpy(tx_frame.msg.data, data, data_length);

	uint32_t isler_frame_length = len + 2;

	// printf("total frame length: %d, Data length: %d, ad_len: %d\n", isler_frame_length, len, tx_frame.msg.hdr.ad_len);

	iSLERTX(ACCESS_ADDRESS, (uint8_t*)&tx_frame, isler_frame_length, SYNTHPASS_CHANNEL, SYNTHPASS_PHY_MODE);

	return 0;
}

uint8_t synthpass_broadcast() {

	uint8_t status = synthpass_tx(SYNTHPASS_BROADCAST, 0, 0);
	if(status == 0) {
		printf("beep!\n");
	} else {
		printf("sad beep :(\n");
	}
	last_broadcast_tick = funSysTick32();
	return status;
}

void printf_uid(uint32_t uid) {
	for(int i = 0; i < 4; ++i) {
		printf(":%02x", (synthpass_uid >> (8 * i)) & 0xFF);
	}
}

uint8_t synthpass_init() {
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


	// debugging - print entire 64 bit UID
	// printf("uid64   ");
	// for(int i = 0; i < (64/8); ++i) {
	// 	printf(":%02x", *(uint8_t*)(0x3F018 + i));
	// }
	// printf("\n");

	// set last broadcast time to now
	last_broadcast_tick = funSysTick32();
	broadcast_random_ticks = 0;

	// first broadcast
	synthpass_broadcast();
	// start listening for frames
	synthpass_rx();

	return 0;
}

uint8_t validate_synthpass_frame(volatile SynthPass_Frame_T *frame) {
	return (
		frame->pdu == SYNTHPASS_PDU
		&& (strncmp((const char*)(frame->mac), SYNTHPASS_MAC, SYNTHPASS_MAC_SIZE) == 0)
	) ? 1 : 0;
}

void incoming_frame_handler() {
	// The chip stores the incoming frame in LLE_BUF, defined in extralibs/iSLER.h
	volatile SynthPass_Frame_T *frame = (volatile SynthPass_Frame_T*)LLE_BUF;



	// check if the RX'd frame is a synthpass frame (PDU and MAC match)
	if(!validate_synthpass_frame(frame)) {
		return;
	}

	// Print frame info
	int rssi = iSLERRSSI();
	int corrected_rssi = rssi - frame->msg.hdr.ref_rssi - SYNTHPASS_REF_RXRSSI;

	printf("RX'd! RSSI:%d PDU:%d len:%d MAC", rssi, frame->pdu, frame->length);
	for(int i = 0; i < SYNTHPASS_MAC_SIZE; ++i) {
		printf(":%02x", frame->mac[i]);
	}
	printf("\n");

	SynthPass_MessageType_T type = frame->msg.hdr.msg_type;

	switch(type) {
		case SYNTHPASS_BROADCAST:
			{
				// received broadcast data from another SynthPass, reply with PROX frame
				printf("BROADCAST peer uid");
				printf_uid(frame->msg.hdr.sender_uid); // TODO for some reason this is the own device's UID rather than the peer's????? 
				printf("\n");
				
				SynthPass_Prox_T msg = {
					.peer_uid=frame->msg.hdr.sender_uid,
					.rx_rssi=corrected_rssi
				};

				synthpass_tx(SYNTHPASS_PROX, (uint8_t*)&msg, sizeof(msg));

				// add sender_uid to peers
				
				// switch to faster message rate
				if(period == SYNTHPASS_BROADCAST_PERIOD) period = SYNTHPASS_PROX_PERIOD;
				synthpass_rx();
			}
			break;
		case SYNTHPASS_PROX:
			{
				// Received a response
				SynthPass_Prox_T *rxData = (SynthPass_Prox_T *) frame->msg.data;
				if(rxData->peer_uid == synthpass_uid) {
					printf("PROX peer uid");
					printf_uid(rxData->peer_uid);
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

int main()
{
	SystemInit();

	funGpioInitAll();
	funPinMode( LED, GPIO_CFGLR_OUT_2Mhz_PP );

	synthpass_init();

	iSLERInit(LL_TX_POWER_0_DBM);

	blink(5);

	while(1) {
		if(rx_ready) {
			incoming_frame_handler();
			synthpass_rx();
		}

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
}

