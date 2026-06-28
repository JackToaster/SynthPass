/*
 * Host-editable config persisted in a dedicated 4 KB sector of the message store.
 * Read into a cached Config_T at boot; rewritten when the host edits
 * ME/synthpass.ini. The synthesized .ini text the host reads is rebuilt from
 * the cached struct after every parse, so host edits always round-trip through
 * the typed values -- comments and unknown keys aren't preserved.
 */
#include "config.h"
#include "msgstore.h"
#include <string.h>

#define DEFAULT_BOOP_RSSI_ADJUST 0
#define DEFAULT_LED_BEHAVIOR     LED_BEHAVIOR_BOOP
#define DEFAULT_DATA_TRIGGER     DATA_TRIGGER_BOOP_ONLY
#define DEFAULT_PROTOCOL_MODE    PROTOCOL_GPIO

static Config_T cached;
static uint8_t  rendered_buf[384];   // synthesized .ini text; ~200 B suffices
static uint16_t rendered_len_cached;

// ---- rendering helpers ----
static uint16_t put_str(uint16_t pos, const char *s) {
	while (*s && pos < sizeof(rendered_buf)) rendered_buf[pos++] = *s++;
	return pos;
}

static uint16_t put_int(uint16_t pos, int v) {
	if (v < 0) {
		if (pos < sizeof(rendered_buf)) rendered_buf[pos++] = '-';
		v = -v;
	}
	char tmp[12]; int n = 0;
	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n > 0 && pos < sizeof(rendered_buf)) rendered_buf[pos++] = tmp[--n];
	return pos;
}

static void rerender(void) {
	uint16_t p = 0;
	p = put_str(p, "# SynthPass config - Save your modified settings and reboot the SynthPass for them to take effect. Comments are not preserved.\n");
	p = put_str(p, "boop_rssi_adjust=");  p = put_int(p, cached.boop_rssi_adjust); p = put_str(p, "  # dB offset on the boop threshold; negative = easier boop\n");
	p = put_str(p, "led_behavior=");      p = put_int(p, cached.led_behavior);     p = put_str(p, "     # 0=on while booped, 1=on while peer in range, 2=always off\n");
	p = put_str(p, "data_trigger=");      p = put_int(p, cached.data_trigger);     p = put_str(p, "     # 0=send on prox and boop, 1=send on boop only\n");
	p = put_str(p, "protocol_mode=");     p = put_int(p, cached.protocol_mode);    p = put_str(p, "    # 0=gpio, 1=i2c (future), 2=serial (future)\n");
	rendered_len_cached = p;
}

// ---- parser ----
// Skip leading whitespace; return adjusted pointer + len.
static void trim_left(const char **s, uint16_t *len) {
	while (*len > 0 && (**s == ' ' || **s == '\t')) { (*s)++; (*len)--; }
}

// Parse a decimal integer (with optional sign). Stops at first non-digit.
// Returns 1 + writes *out on success, 0 if no digits.
static int parse_int(const char *s, uint16_t len, int *out) {
	trim_left(&s, &len);
	int sign = 1;
	if (len > 0 && *s == '-') { sign = -1; s++; len--; }
	int v = 0, n = 0;
	while (len > 0 && *s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; len--; n++; }
	if (n == 0) return 0;
	*out = v * sign;
	return 1;
}

// Returns nonzero if `s` (length `slen`) equals `lit` (NUL-terminated).
static int key_is(const char *s, uint16_t slen, const char *lit) {
	uint16_t i = 0;
	while (lit[i] && i < slen && s[i] == lit[i]) i++;
	return lit[i] == 0 && i == slen;
}

static void apply_line(const char *line, uint16_t len, Config_T *cfg) {
	trim_left(&line, &len);
	if (len == 0 || *line == '#' || *line == ';') return;

	// Find '='
	uint16_t eq = 0;
	while (eq < len && line[eq] != '=') eq++;
	if (eq == len) return;

	uint16_t klen = eq;
	while (klen > 0 && (line[klen-1] == ' ' || line[klen-1] == '\t')) klen--;
	const char *vstart = line + eq + 1;
	uint16_t vlen = len - eq - 1;

	int v;
	if (!parse_int(vstart, vlen, &v)) return;

	if      (key_is(line, klen, "boop_rssi_adjust")) cfg->boop_rssi_adjust = (int8_t)v;
	else if (key_is(line, klen, "led_behavior"))     cfg->led_behavior     = (uint8_t)v;
	else if (key_is(line, klen, "data_trigger"))     cfg->data_trigger     = (uint8_t)v;
	else if (key_is(line, klen, "protocol_mode"))    cfg->protocol_mode    = (uint8_t)v;
}

static void set_defaults(Config_T *c) {
	c->boop_rssi_adjust = DEFAULT_BOOP_RSSI_ADJUST;
	c->led_behavior     = DEFAULT_LED_BEHAVIOR;
	c->data_trigger     = DEFAULT_DATA_TRIGGER;
	c->protocol_mode    = DEFAULT_PROTOCOL_MODE;
}

// ---- public API ----
void config_init(void) {
	if (!msgstore_config_read(&cached, sizeof(cached))) set_defaults(&cached);
	rerender();
}

const Config_T *config_get(void) { return &cached; }

void config_apply_text(const uint8_t *buf, uint16_t len) {
	// Start from the cached values so unrecognized keys stay put.
	Config_T parsed = cached;
	uint16_t line_start = 0;
	for (uint16_t i = 0; i <= len; i++) {
		if (i == len || buf[i] == '\n' || buf[i] == '\r') {
			if (i > line_start) apply_line((const char*)(buf + line_start), (uint16_t)(i - line_start), &parsed);
			line_start = (uint16_t)(i + 1);
		}
	}
	cached = parsed;
	msgstore_config_write(&cached, sizeof(cached));
	rerender();
}

const uint8_t *config_rendered(void)     { return rendered_buf; }
uint16_t       config_rendered_len(void) { return rendered_len_cached; }
