#ifndef RADIO_H
#define RADIO_H

// SynthPass 2.4GHz radio + protocol, built on ch32fun's iSLER (BLE-advertising
// frames). Detects peers and exchanges proximity/boop messages. Debug output
// goes to the CDC link via printf (see usb.c).

// Initialize protocol state + the iSLER radio, and start broadcasting/listening.
void radio_init(void);

// Service received frames and periodic broadcasting; call regularly from main.
void radio_task(void);

#endif // RADIO_H
