#ifndef RADIO_H
#define RADIO_H
#include "synthpass.h"
// SynthPass 2.4GHz radio + protocol, built on ch32fun's iSLER (BLE-advertising
// frames). Detects peers and exchanges proximity/boop messages. Debug output
// goes to the CDC link via printf (see usb.c).

// Initialize protocol state + the iSLER radio, and start broadcasting/listening.
void radio_init(void);

// Stop the iSLER radio. Call before jumping to the ROM bootloader.
void radio_shutdown(void);

// Service received frames and periodic broadcasting; call regularly from main.
void radio_task(SynthPass_PeerState_T *peers);

// check if any peer is in the booped state
uint8_t is_any_peer_booped(SynthPass_PeerState_T *peers);

#endif // RADIO_H
