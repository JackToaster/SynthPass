#ifndef SYNTHPASS_CONFIG_H
#define SYNTHPASS_CONFIG_H

#include <stdint.h>

// Host-editable parameters, persisted in a dedicated 4 KB sector inside the
// message store (see msgstore.c CONFIG_OFF). Read into the cached struct at
// boot; rewritten when the host edits ME/synthpass.ini and unmounts.
//
// Enum members are kept as uint8_t (rather than typed enums) so the cross-
// boundary struct stays 4 bytes and survives flash round-trip cleanly.

// led_behavior values:
#define LED_BEHAVIOR_BOOP   0  // on while any peer is booped (default)
#define LED_BEHAVIOR_PROX   1  // on while any peer is in proximity range
#define LED_BEHAVIOR_OFF    2  // always off

// data_trigger values:
#define DATA_TRIGGER_PROX_AND_BOOP 0  // send PROX_DATA and BOOP_DATA (default)
#define DATA_TRIGGER_BOOP_ONLY     1  // only send data on boop

// protocol_mode values:
#define PROTOCOL_GPIO     0  // PA2/PA3 as discrete GPIO (default)
#define PROTOCOL_I2C      1  // (future)
#define PROTOCOL_SERIAL   2  // (future)

typedef struct {
	int8_t  boop_rssi_adjust;  // dB offset added to BOOP_RSSI threshold (negative = easier boop)
	uint8_t led_behavior;
	uint8_t data_trigger;
	uint8_t protocol_mode;
} Config_T;

// Load cached config from flash; falls back to defaults if not initialized.
// Call once at boot, before USB or radio start.
void config_init(void);

// Pointer to the live config. Cheap, no I/O.
const Config_T *config_get(void);

// Replace the cached config by parsing `buf` (host wrote a new ME/synthpass.ini).
// Persists to flash and re-renders the synthesized .ini text. Unknown keys and
// malformed lines are silently dropped; unrecognized values fall back to the
// existing cached field.
void config_apply_text(const uint8_t *buf, uint16_t len);

// Synthesized .ini text for MSC reads of ME/synthpass.ini. Stable until the
// next config_init / config_apply_text. (Comments in host edits are NOT
// preserved -- the rendered text always reflects the current parsed values.)
const uint8_t *config_rendered(void);
uint16_t       config_rendered_len(void);

#endif
