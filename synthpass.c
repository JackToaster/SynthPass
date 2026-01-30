/* 
 * Minimal demo of iSLER with transmit and receive, on configurable PHY (1M, 2M, S2 or S8 if supported by the mcu)
 * It listens for advertisements from other BLE devices, and when one is detected it
 * changes it's own "Complete Local Name" to RX:XX where XX is the first byte of the detected BLE device's MAC.
 * The RX process happens on channel 37 AccessAddress 0x8E89BED6, which is defined in extralibs/iSLER.h.
 * When a new frame is received, the callback "incoming_frame_handler()" is called to process it.
 */
#include "ch32fun.h"
#include "iSLER.h"
#include <stdio.h>
#include <string.h>

#include "synthpass.h"

#define LED PA11


__attribute__((aligned(4))) SynthPass_Frame_T tx_frame = {};
uint32_t synthpass_uid;

void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		funDigitalWrite( LED, FUN_LOW ); // Turn on LED
		Delay_Ms(33);
		funDigitalWrite( LED, FUN_HIGH ); // Turn off LED
		if(i) Delay_Ms(33);
	}
}

uint8_t synthpass_init() {
	// Get our UID, based on the chip's built-in ID
	uint64_t chip_uid = *(uint64_t*)(0x3F018);
	synthpass_uid = (chip_uid >> 32) ^ (chip_uid & 0xFFFFFFFF); 

	tx_frame.pdu = 0x02;
	tx_frame.length = SYNTHPASS_MAC_SIZE;
	strncpy((char*) tx_frame.mac, SYNTHPASS_MAC, SYNTHPASS_MAC_SIZE);
	tx_frame.msg.hdr.ref_rssi = SYNTHPASS_REF_RSSI;
	tx_frame.msg.hdr.sender_uid = synthpass_uid;

	printf("Synthpass init! UID");
	for(int i = 0; i < 4; ++i) {
		printf(":%02x", (synthpass_uid >> (8 * i)) & 0xFF);
	}
	printf("\n");


	// debugging - print entire 64 bit UID
	// printf("uid64   ");
	// for(int i = 0; i < (64/8); ++i) {
	// 	printf(":%02x", *(uint8_t*)(0x3F018 + i));
	// }
	// printf("\n");

	return 0;
}

uint8_t synthpass_tx(SynthPass_MessageType_T type, uint8_t* data, uint8_t data_length) {
	uint32_t len = data_length + sizeof(SynthPass_Header_T) + SYNTHPASS_MAC_SIZE;
	if(len > 255) {
		return 1;
	}
	tx_frame.length = data_length + sizeof(SynthPass_Header_T) + SYNTHPASS_MAC_SIZE;
	
	return 0;
}

uint8_t validate_synthpass_frame(SynthPass_Frame_T *frame) {
	return (
		frame->pdu == SYNTHPASS_PDU
		&& (strncmp((const char*)(frame->mac), SYNTHPASS_MAC, SYNTHPASS_MAC_SIZE) == 0)
	) ? 1 : 0;
}

void incoming_frame_handler() {
	// The chip stores the incoming frame in LLE_BUF, defined in extralibs/iSLER.h
	SynthPass_Frame_T *frame = (SynthPass_Frame_T*)LLE_BUF;

	// check if the RX'd frame is a synthpass frame (PDU and MAC match)
	if(!validate_synthpass_frame(frame)) {
		return;
	}

	int rssi = iSLERRSSI();

	// Print frame info
	printf("RX'd! RSSI:%d PDU:%d len:%d MAC", rssi, frame->pdu, frame->length);
	
	for(int i = 0; i < SYNTHPASS_MAC_SIZE; ++i) {
		printf(":%02x", frame->mac[i]);
	}

	printf("\n");
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
		// printf("beep!\n");
		// iSLERTX(ACCESS_ADDRESS, (uint8_t*) &tx_frame, sizeof(tx_frame), SYNTHPASS_CHANNEL, PHY_MODE);
		
		// blink(5);

		// for(uint32_t i = 0; i < 50; ++i){
		
		iSLERRX(ACCESS_ADDRESS, SYNTHPASS_CHANNEL, PHY_MODE);

		while(!rx_ready) {}
		incoming_frame_handler();
		// for(uint32_t i = 0; i < 500; ++i) {
		// 	if(rx_ready) {
		// 		incoming_frame_handler();
		// 		iSLERRX(ACCESS_ADDRESS, SYNTHPASS_CHANNEL, PHY_MODE);
		// 	}
		// 	Delay_Ms(1);
		// }
	}
}

