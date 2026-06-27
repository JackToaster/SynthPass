#ifndef BOARD_H
#define BOARD_H

#include "ch32fun.h"

// Per-board pin map / behavior. The BOARD_* macro is set in each env's
// build_flags in platformio.ini.

// SYNTHPASS_REF_RSSI / SYNTHPASS_REF_RXRSSI are this board's RSSI calibration
// against the NanoCH57x reference: REF_RSSI is broadcast in each frame as our
// TX calibration ("how this board would appear at 1m on a reference receiver"),
// and REF_RXRSSI is subtracted from raw RSSI on receive to normalize against
// the reference RX sensitivity. NanoCH57x is the reference, so it -- and the
// generic dev board, which we treat as identical for now -- get 0/0.

#if defined(BOARD_GENERIC_CH572Q)
    // CH572Q dev board: active-low LED on PA11.
    #define LED_PIN   PA11
    #define LED_ON()  funDigitalWrite(LED_PIN, FUN_LOW)
    #define LED_OFF() funDigitalWrite(LED_PIN, FUN_HIGH)

    #define SYNTHPASS_REF_RSSI    0
    #define SYNTHPASS_REF_RXRSSI  0

#elif defined(BOARD_NANOCH57X)
    // NanoCH57x dev board (RSSI calibration reference).
    #define LED_PIN   PA11
    #define LED_ON()  funDigitalWrite(LED_PIN, FUN_LOW)
    #define LED_OFF() funDigitalWrite(LED_PIN, FUN_HIGH)

    #define SYNTHPASS_REF_RSSI    0
    #define SYNTHPASS_REF_RXRSSI  0

#elif defined(BOARD_PRETZELS_LAB_R1)
    // Official Pretzel's Lab SynthPass R1 board.
    // Active High LED on PA10
    #define LED_PIN   PA10
    #define LED_ON()  funDigitalWrite(LED_PIN, FUN_HIGH)
    #define LED_OFF() funDigitalWrite(LED_PIN, FUN_LOW)

    #define SYNTHPASS_REF_RSSI   -3
    #define SYNTHPASS_REF_RXRSSI -3

    // QWIIC port outputs: PA2 (SDA) high while any peer is in proximity range,
    // PA3 (SCL) high while any peer is booped. Push-pull, active high.
    #define HAVE_QWIIC_GPIO
    #define QWIIC_PROX_PIN PA2
    #define QWIIC_BOOP_PIN PA3

#elif defined(BOARD_AXION_QC_R1)
    // AxionQC's SynthPass hardware.
    #error "AxionQC hardware not implemented yet!"
#else
    #error "No BOARD_* macro defined; check platformio.ini env build_flags."
#endif

#endif // BOARD_H
