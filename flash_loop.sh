#!/usr/bin/env bash
# Interactive SynthPass flasher.
# Detects a board plugged in (running or in ISP), flashes it, shows a
# confirmation, and loops waiting for the next board. Ctrl-C to quit.
#
# Usage:
#   ./flash_loop.sh [env]      # env defaults to pretzelslab
set -o pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ENV="${1:-pretzelslab}"
BIN="$HERE/.pio/build/$ENV/firmware.bin"
MINICHLINK="$HOME/.platformio/packages/tool-minichlink/minichlink"

SYNTHPASS_VID_PID="1209:d035"   # running app (USB CDC)
ISP_VID_PID="1a86:55e0"         # WCH ISP bootloader
LEDGER="$HERE/flashed_devices.tsv"

# ---- TUI helpers ----------------------------------------------------------
RST=$'\e[0m'
BLD=$'\e[1m'
DIM=$'\e[2m'
RED=$'\e[31m'
GRN=$'\e[32m'
YEL=$'\e[33m'
CYA=$'\e[36m'
GRY=$'\e[90m'

frame() {
	clear
	printf "%s┌─────────────────────────────────────────┐%s\n" "$CYA$BLD" "$RST"
	printf "%s│  SynthPass flasher  ·  env: %-10s │%s\n" "$CYA$BLD" "$ENV" "$RST"
	printf "%s└─────────────────────────────────────────┘%s\n" "$CYA$BLD" "$RST"
	printf "%sCtrl-C to quit. Flashed boards counted below.%s\n" "$GRY" "$RST"
	printf "%sFlashed this session: %s%d%s\n\n" "$GRY" "$BLD$GRN" "$count_ok" "$RST"
}

status() { printf "%s%s%s\n" "$1" "$2" "$RST"; }

# ---- USB introspection ----------------------------------------------------
# Bus paths (e.g. "1-2") of all currently-connected SynthPass-related devices,
# both app-mode (1209:d035) and ISP-mode (1a86:55e0). One per line.
synthpass_bus_paths() {
	local d v p
	for d in /sys/bus/usb/devices/*; do
		[ -f "$d/idVendor" ] || continue
		v=$(cat "$d/idVendor" 2>/dev/null)
		p=$(cat "$d/idProduct" 2>/dev/null)
		if { [ "$v" = "1209" ] && [ "$p" = "d035" ]; } || { [ "$v" = "1a86" ] && [ "$p" = "55e0" ]; }; then
			basename "$d"
		fi
	done | sort -u
}

# State of the device at a given bus path: "1209:d035" / "1a86:55e0" / "" (gone)
device_state() {
	local d="/sys/bus/usb/devices/$1"
	[ -f "$d/idVendor" ] || { echo ""; return; }
	echo "$(cat "$d/idVendor"):$(cat "$d/idProduct")"
}

isp_present() { lsusb | grep -q "$ISP_VID_PID"; }

# Current /dev/ttyACMx for a board at a given bus path, "" if none.
port_of_bp() {
	local p d bp=$1
	for p in /dev/ttyACM*; do
		[ -e "$p" ] || continue
		d=$(readlink -f "/sys/class/tty/$(basename "$p")/device" 2>/dev/null)
		while [ -n "$d" ] && [ "$d" != "/" ] && [ ! -e "$d/idVendor" ]; do d=$(dirname "$d"); done
		[ "$(basename "$d")" = "$bp" ] && echo "$p" && return 0
	done
	return 1
}

# Send 'b' over a held-open serial fd so DTR stays asserted -- a bare open/close
# toggles DTR and the byte is often dropped before the firmware reads it.
trigger_isp_via_b() {
	local port=$1
	stty -F "$port" raw -echo 115200 2>/dev/null
	exec 3<>"$port"
	printf 'b' >&3
	# brief drain so the byte makes it across before we close
	(command -v python3 >/dev/null && python3 -c "import termios,sys;termios.tcdrain(3)" 3<>"$port" 2>/dev/null) || true
	exec 3>&-
}

# ---- Flashing -------------------------------------------------------------
# Run `minichlink -P` (one-time read-protect setting needed for fresh chips to
# run user code) and then the actual flash. minichlink prints to stdout/stderr;
# we keep its output in a log and surface only the failure tail on error.
LOG=/tmp/synthpass_flash.log
flash_isp_device() {
	"$MINICHLINK" -C isp -P            >"$LOG" 2>&1 || true
	"$MINICHLINK" -C isp -w "$BIN" flash -b >>"$LOG" 2>&1
}

# Parse "Part UUID: aa-bb-cc-dd-ee-ff-gg-hh" out of the most recent log. The
# chip exposes a 64-bit UID at 0x3F018, printed by minichlink byte-by-byte in
# the order the chip reads them (little-endian). The radio's synthpass_uid is
# (high32 XOR low32) of that LE u64 -- the value users see as `:xx:xx:xx:xx`
# in MESSAGES.HTM peer prefixes (also LE-printed).
extract_part_uuid() { grep -oE 'Part UUID: [0-9a-f-]+' "$LOG" | tail -1 | awk '{print $3}'; }

compute_synthpass_uid() {
	# Input: dash-separated 8 hex bytes, e.g. "8e-17-f2-e5-66-e4-e6-e1".
	# bytes[0..3] = LE low32, bytes[4..7] = LE high32.
	# Returns the LE-printed UID in :xx:xx:xx:xx form so it lines up with how
	# users see it in MESSAGES.HTM.
	local b
	IFS='-' read -r -a b <<<"$1"
	[ "${#b[@]}" -eq 8 ] || { echo "?"; return; }
	local low=$((  0x${b[0]} | (0x${b[1]} << 8) | (0x${b[2]} << 16) | (0x${b[3]} << 24) ))
	local high=$(( 0x${b[4]} | (0x${b[5]} << 8) | (0x${b[6]} << 16) | (0x${b[7]} << 24) ))
	local uid=$(( low ^ high ))
	printf ":%02x:%02x:%02x:%02x" $((uid & 0xff)) $(((uid >> 8) & 0xff)) $(((uid >> 16) & 0xff)) $(((uid >> 24) & 0xff))
}

ledger_record() {
	# args: bus_path  part_uuid  synthpass_uid
	if [ ! -f "$LEDGER" ]; then
		printf "timestamp\tenv\tbus_path\tpart_uuid\tsynthpass_uid\n" >"$LEDGER"
	fi
	printf "%s\t%s\t%s\t%s\t%s\n" "$(date -Iseconds)" "$ENV" "$1" "$2" "$3" >>"$LEDGER"
}

# ---- Build the firmware if missing ---------------------------------------
ensure_built() {
	if [ ! -f "$BIN" ]; then
		status "$YEL" "Building $ENV (firmware.bin missing)..."
		pio run -e "$ENV" >/tmp/synthpass_build.log 2>&1 || {
			status "$RED" "Build failed:"
			tail -20 /tmp/synthpass_build.log
			exit 1
		}
	fi
}

# ---- Wait helpers ---------------------------------------------------------
# Wait until a bus path appears in synthpass_bus_paths that's NOT in `seen`.
# Sets the global `new_bp`.
new_bp=""
wait_for_new_device() {
	new_bp=""
	while [ -z "$new_bp" ]; do
		local bp
		for bp in $(synthpass_bus_paths); do
			if ! printf "%s\n" "${seen[@]:-}" | grep -qFx "$bp"; then
				new_bp=$bp
				return 0
			fi
		done
		sleep 0.3
	done
}

# Wait for a given bus path to be physically unplugged. Has to be debounced:
# the ISP -> app re-enumeration after a flash briefly drops the device from the
# bus, and naive "is bp in lsusb" would interpret that gap as an unplug, kick
# us back to the main loop, and the script would re-flash the board it just
# flashed. Require N consecutive polls of "gone" before believing it.
wait_for_unplug() {
	local bp=$1 absent=0
	# 6 * 0.3s = ~1.8s of continuous absence — covers the USB drop-out
	# during a post-flash reset on the CH572 (well under a second in practice).
	while [ "$absent" -lt 6 ]; do
		if synthpass_bus_paths | grep -qFx "$bp"; then
			absent=0
		else
			absent=$((absent + 1))
		fi
		sleep 0.3
	done
}

# ---- Main -----------------------------------------------------------------
count_ok=0
seen=()
ensure_built

while true; do
	frame
	status "$CYA" "Waiting for a board to plug in..."
	wait_for_new_device
	bp=$new_bp

	frame
	status "$GRN" "● Detected on bus $bp"

	state=$(device_state "$bp")
	if [ "$state" = "1209:d035" ]; then
		port=$(port_of_bp "$bp") || port=""
		if [ -n "$port" ]; then
			status "$YEL" "  app mode → sending 'b' to enter ISP via $port"
			trigger_isp_via_b "$port"
			for _ in $(seq 1 30); do isp_present && break; sleep 0.3; done
		fi
	fi

	if ! isp_present; then
		status "$RED" "✗ couldn't reach ISP bootloader. Skipping."
		sleep 3
		seen+=("$bp")
		continue
	fi

	status "$YEL" "  one-time setup (minichlink -P)..."
	"$MINICHLINK" -C isp -P >"$LOG" 2>&1 || true
	# -P sometimes reboots the chip; wait for ISP to be alive again.
	for _ in $(seq 1 20); do isp_present && break; sleep 0.3; done

	status "$YEL" "  flashing $BIN..."
	if "$MINICHLINK" -C isp -w "$BIN" flash -b >>"$LOG" 2>&1; then
		count_ok=$((count_ok + 1))
		part_uuid=$(extract_part_uuid)
		synth_uid=$(compute_synthpass_uid "$part_uuid")
		ledger_record "$bp" "$part_uuid" "$synth_uid"
		frame
		printf "%s%s┌─────────────────────────────┐%s\n" "$GRN" "$BLD" "$RST"
		printf "%s%s│  ✓  FLASH COMPLETE           │%s\n" "$GRN" "$BLD" "$RST"
		printf "%s%s└─────────────────────────────┘%s\n" "$GRN" "$BLD" "$RST"
		echo
		status "$DIM" "  bus $bp · $ENV"
		status "$DIM" "  Part UUID:     $part_uuid"
		status "$DIM" "  Synthpass UID: $synth_uid"
		status "$DIM" "  Logged to:     $LEDGER"
		echo
		status "$CYA" "Unplug the board, then plug in the next one."
		seen+=("$bp")
		wait_for_unplug "$bp"
		# Forget this bus path so a re-insertion at the same port is welcome.
		new_seen=()
		for s in "${seen[@]}"; do [ "$s" != "$bp" ] && new_seen+=("$s"); done
		seen=("${new_seen[@]:-}")
	else
		status "$RED" "✗ flash failed:"
		tail -10 "$LOG" | sed 's/^/    /'
		echo
		status "$DIM" "Unplug to retry, or Ctrl-C to quit."
		seen+=("$bp")
		wait_for_unplug "$bp"
		# Drop from seen so a re-plug is a fresh attempt.
		new_seen=()
		for s in "${seen[@]}"; do [ "$s" != "$bp" ] && new_seen+=("$s"); done
		seen=("${new_seen[@]:-}")
	fi
done
