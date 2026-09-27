/*
 * SynthPass -- firmware entry point.
 *
 * A protogen/synth nose-boop gadget on the WCH CH572. Subsystems:
 *   - radio.c    : 2.4GHz iSLER radio + SynthPass protocol (peer detect, PROX)
 *   - usb.c      : composite USB -- CDC debug serial + read-only MSC mass storage
 *   - msgstore.c : flash-backed message store (read by the MSC layer)
 *
 * This file wires them together and runs the main loop.
 */
#include "ch32fun.h"
#include "ch5xxhw.h"   // jump_isprom
#include <stdio.h>     // putchar (defined in usb.c)
#include "fsusb.h"
#include "radio.h"
#include "synthpass.h"
#include "board.h"

SynthPass_PeerState_T peers[MAX_PEERS];

void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		LED_ON();
		Delay_Ms(33);
		LED_OFF();
		if(i) Delay_Ms(33);
	}
}


// uint8_t cdc_input_buf[512]; // TODO what's an appropritate buffer size? Shoudl be big enough for any companion protocol command we support

void handle_debug_input( int numbytes, uint8_t * data )
{
	if(data[0] == 'b') {
		blink(5);
		radio_shutdown();
		jump_isprom();
	}
}


int main()
{
	SystemInit();

	funGpioInitAll();
	funPinMode( LED_PIN, GPIO_CFGLR_OUT_10Mhz_PP );
#ifdef HAVE_QWIIC_GPIO
	funPinMode( QWIIC_PROX_PIN, GPIO_CFGLR_OUT_10Mhz_PP );
	funPinMode( QWIIC_BOOP_PIN, GPIO_CFGLR_OUT_10Mhz_PP );
	funDigitalWrite( QWIIC_PROX_PIN, FUN_LOW );
	funDigitalWrite( QWIIC_BOOP_PIN, FUN_LOW );
#endif

	radio_init();    // protocol state + iSLER radio; start broadcasting

	blink(1);

	while(1) {
		radio_task(peers);  // radio rx + periodic broadcast
	}
}
