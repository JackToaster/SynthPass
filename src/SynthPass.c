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

#include "usb.h"
#include "msgstore.h"
#include "radio.h"
#include "config.h"
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

	msgstore_init(); // seed the message store into flash if needed
	config_init();   // load host-editable config (or defaults) from its sector
	usb_init();      // bring up CDC + MSC
	radio_init();    // protocol state + iSLER radio; start broadcasting

	blink(1);

	while(1) {
		// CDC debug input: 'b' jumps to ROM bootloader, anything else is echoed.
		int c = usb_cdc_getc();
		if(c >= 0) {
			if(c == 'b') {
				blink(5);
				usb_reset();
				jump_isprom(); // enters the USB ISP bootloader; does not return
			} else {
				putchar(c);
			}
		}

		usb_task();    		// handle MSC transfer. returns immediately if no usb transfer happening.
		radio_task(peers);  // radio rx + periodic broadcast
	}
}
