# Integrating WCH's Real BLE Stack (vs. raw iSLER)

Investigation into what it would take to get connectable BLE + GATT working on the
CH572, as a replacement for or addition to the current iSLER-based raw-PHY radio in
`src/radio.c`. Short version: the stack exists and there is prior art for linking it
outside MounRiver, but this project's flash layout is the real blocker, not the
toolchain.

---

## 1. What iSLER actually is (and isn't)

Confirmed by reading `extralibs/iSLER.h` and the `examples_ch5xx/iSLER` /
`ble_beacon` examples in the pinned `framework-ch32v003fun` package: iSLER is a thin
wrapper around the CH57x/CH58x/CH59x baseband+link-layer *registers* (`BB`, `LL`,
`RF` MMIO blocks). It exposes exactly two operations — `iSLERTX()` and `iSLERRX()` —
that send or receive one raw link-layer PDU on one channel with one access address.
There is no connection state machine, no channel-hopping, no CONNECT_IND handling,
no encryption, no ATT/GATT. It's a building block for advertising/scanning-shaped
traffic (which is exactly what this project's mesh protocol currently uses it for),
not a BLE stack. Getting actual connectable BLE (a phone doing a GATT read, say)
requires a real link layer + host stack on top of this hardware, which iSLER does
not provide and was never meant to.

## 2. What WCH ships for BLE, and whether CH572 is covered

WCH's official BLE stack is a **closed-source precompiled static library**, shipped
as part of each chip family's EVT (their MounRiver-based SDK/example package), not
as source. Confirmed directly from `openwch/ch570` (the GitHub EVT mirror that
covers CH570 *and* CH572 — the two share one datasheet and one EVT):

- `EVT/EXAM/BLE/LIB/libCH572BLE_PERI.a` — 612 KB archive, CH572-specific, `_PERI`
  suggests peripheral-role-only (no central/scanner role) — consistent with a
  cost/RAM-optimized chip.
- Startup files (`startup_CH572.S`), a shared `Link.ld` (240 KB FLASH / 12 KB RAM
  regions, matching the datasheet), and full example projects: `Peripheral`,
  `HID_Mouse`, `HID_Consumer`, `SpeedTest_Peripheral`, `BackupUpgrade_OTA`, plus a
  `Profile/gattprofile.c` GATT service example.
- The equivalent for CH59x is `libCH59xBLE.a` (~1.1 MB archive, per the
  `openwch/ch592` EVT) and for CH58x it's referred to elsewhere as
  `LibWCHBLE.a`/`libCH58xBLE.a` — same pattern across the whole family, just
  different archive per chip.

So: yes, CH572 has an official BLE stack, it is not a CH571/573/582-only thing, and
it comes from the same `openwch/ch570` EVT repo already worth pulling for reference
(the `docs/CH572DS1_en-3.PDF` datasheet's "Provide optimized protocol stack and
application layer API" line is this library).

The stack requires **TMOS**, WCH's small cooperative RTOS, as its runtime substrate
(task callbacks/events, not a hard dependency on a specific scheduler — see §3).
Datasheet also confirms BLE 5.0 compliance, GFSK 1M/2M PHY, -95 dBm RX sensitivity.

## 3. Linking it outside MounRiver / against ch32fun

This is a solved problem for sibling chips, not a novel one:

- **CH592**: [rgoulter's writeup](https://rgoulter.com/blog/posts/programming/2023-12-21-notes-on-building-wch-ble-ch592-evt-exam-with-bare-makefile.html)
  and the accompanying [ch592-ble-hid-keyboard-example](https://github.com/rgoulter/ch592-ble-hid-keyboard-example)
  repo build a WCH BLE EVT example (vendored `sdk/` tree, including
  `libCH59xBLE.a` + `libISP592.a`) with a bare Makefile and either the MounRiver
  toolchain (`riscv-none-embed-gcc 8.2.0`) or a modern upstream xPack
  `riscv-none-elf-gcc`. Confirmed gotchas: newer GCC (12.1+) needs `_zicsr` added
  to `-march` or you get `"extension 'zicsr' required"`; MounRiver's toolchain
  accepts a `-highcode-gen-section-name` flag upstream GCC doesn't recognize;
  mismatched `-D` defines between example projects silently produce a
  non-functional build (author lost time to this). None of these are blockers,
  just things to get right — and ch32fun's build already carries its own
  `-march=`/toolchain handling for these chips, so the same class of fix likely
  already applies or is a small patch.
- **CH32V208 (Rust, not C, but same closed blob)**: the
  [ch32-hal BLE discussion](https://github.com/ch32-rs/ch32-hal/discussions/30)
  confirms the library is closed-source (`LibWCHBLE.a` in their EVT), requires
  TMOS, but that TMOS's task-callback/event model is loose enough that the
  `ch58x-hal` project got it running glued into an entirely different runtime
  (Embassy, an async Rust executor) rather than TMOS itself — i.e. the blob does
  not require literally using WCH's own main loop / scheduler source, just
  satisfying the calling contract it expects. No blockers were reported beyond
  "it's proprietary and you have to be careful with the RTOS glue."

Net: nobody has hit a hard wall linking these blobs against non-vendor-SDK builds.
It's static-library integration + reimplementing (or vendoring) enough of TMOS's
task/event glue and the HAL calls (`HAL/MCU.c`, `HAL/RTC.c`, `HAL/SLEEP.c` etc. in
the EVT) that the blob calls into. That glue is on the order of a few hundred
lines per the EVT `HAL/` directory, not a full RTOS port.

Vendoring convention already exists in this repo for exactly this shape of
problem: `lib/ed25519/` and `lib/sha256/` are both vendored third-party C, dropped
into their own `lib/<name>/` directory, picked up automatically by PlatformIO's
Library Dependency Finder. `libCH572BLE_PERI.a` + `CH59xBLE_LIB.h`-equivalent
headers + the EVT's `HAL/`/TMOS glue would follow the same pattern — except this
is a **prebuilt binary blob**, not source, so `lib/wch_ble/` would hold the `.a`
file, WCH's headers, and only the small amount of glue code this project has to
write itself, with a comment citing where the `.a` came from (which EVT
tag/commit) since there's no upstream to point PlatformIO at for auto-fetch.

**Toolchain-ABI risk not fully verified here**: the blob is compiled by WCH with
their own GCC (MounRiver ships 8.2.0 per rgoulter's post). ch32fun's pinned
toolchain version wasn't checked against this — a static-library blob generally
only cares about matching calling convention / ABI (RISC-V ilp32 or similar, plus
whatever `-march`/`-mabi` WCH built it with), not exact GCC version, but this
needs hands-on verification (a link failure or an `-march` mismatch would show up
immediately at link time, so this is a fast thing to test, just untested so far).

## 4. Flash/RAM footprint vs. what this project has left — the real blocker

This is the part worth flagging loudest. From `synthpass_ch572.ld`:

```
FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 48K   /* top 192K reserved for the message store */
RAM   (xrw): ORIGIN = 0x20000000, LENGTH = 12K
```

The CH572 has 240 KB usable CodeFlash and 12 KB SRAM total (confirmed in both the
datasheet and WCH's own stock `Link.ld`). This project's custom linker script
already gives away 192 KB of that flash to the message store, leaving only **48 KB**
for everything else — bootloader-adjacent glue, the mesh protocol, USB, msgstore
logic, and now potentially a BLE stack.

WCH's own official `BLE/Peripheral` example for CH572 (plain peripheral-role GATT
example, no OTA) — confirmed from the `openwch/ch570` EVT tree — produces a 252,644
byte Intel HEX output. Intel HEX ASCII-encodes each byte as two hex characters plus
per-line framing overhead, so actual flashed binary size is smaller than the raw
hex byte count, but even generously assuming ~2.2x encoding overhead that's still
roughly **110–115 KB of actual flash** for a bare-bones connectable BLE peripheral
with one GATT service — before this project's own application code, USB stack, or
protocol logic are added. That does not fit in 48 KB. It doesn't obviously fit
even if the message-store reservation were cut in half.

This wasn't independently rebuilt/measured (no toolchain set up to actually link
the CH572 EVT project in this investigation) — the number above is inferred from
the shipped `.hex`, not a verified `.map`/`size` report. Treat it as "clearly too
big by a wide margin," not as a precise figure. RAM footprint (arguably the
tighter constraint given only 12 KB total, some of which is presumably already
spoken for by this project's buffers) wasn't findable from public docs at all and
would need to come from an actual link/`.map` file — WCH doesn't publish this
number and neither `openwch/ch570` nor the two blog/discussion sources above state
it.

## 5. Alternatives

- **Open-source BLE stack ported to these WCH RISC-V cores**: none found. The
  radio hardware (`BB`/`LL`/`RF` register blocks documented only by
  reverse-engineering, per ch32fun's own comments in `iSLER.h`: "Unimplemented!",
  "couldn't find it, not sure") is undocumented enough that an independent
  clean-room link-layer implementation is a much larger effort than integrating
  WCH's blob, and nobody appears to have attempted it.
- **Stay on iSLER, don't do real BLE**: given the flash math in §4, this is the
  practical near-term answer — the mesh protocol's raw-advertising approach is
  cheap precisely because it skips the link layer entirely. Real BLE connectability
  is a substantial flash-budget project, not a drop-in library add.
- **Shrink the message store reservation** to make room: mathematically necessary
  either way (48 KB isn't enough headroom even before BLE), but a ~140 KB+ cut
  would gut the store's whole reason for existing. Worth a back-of-envelope check
  against what the store actually needs before assuming it's flexible.

## 6. If this gets pursued anyway: recommended order of operations

1. Pull `openwch/ch570`'s `EVT/EXAM/BLE/Peripheral` example, build it as-is with
   MounRiver (or rgoulter's bare-Makefile approach adapted to CH572) to get a real
   `.map` file — turns the §4 estimate into a verified number before any
   integration work starts. This is cheap and answers the only question that
   actually matters here.
2. If the real flash number confirms it doesn't fit in 48 KB (expected), that's
   the actual gating decision — resolve the flash-budget conflict with the
   message store before writing any BLE integration code, not after.
3. Only then: vendor `libCH572BLE_PERI.a` + WCH's BLE headers into `lib/wch_ble/`
   (matching the `lib/ed25519`/`lib/sha256` convention), port the `HAL/` glue
   (`MCU.c`/`RTC.c`/`SLEEP.c`/`KEY.c`/`LED.c` — all small, all in the EVT) to call
   into ch32fun instead of WCH's peripheral library, and verify link-time ABI
   compatibility between ch32fun's toolchain and the blob (§3's open risk) with a
   trivial "link but don't call anything real yet" smoke test before wiring up
   actual GATT services.
4. Confirm RAM headroom by actual `.map` inspection, not estimate — 12 KB total is
   tight enough that this needs a real number, not a guess, before committing to
   a design that assumes it fits alongside existing buffers.

---

**Confidence notes**: §1–§3 (what iSLER is, that the blob exists for CH572
specifically, that outside-MounRiver linking has working prior art for sibling
chips) are confirmed against primary sources — the vendored ch32fun package, the
`openwch/ch570` EVT tree, and two independent writeups. §4's specific byte counts
are inferred from a shipped `.hex` file, not a rebuilt `.map`, and should be
treated as directional. Licensing: `openwch/ch570` (the repo containing
`libCH572BLE_PERI.a`) carries a top-level `LICENSE` file and it's Apache 2.0 —
which does permit redistributing Object form (i.e. the `.a` blob) under that
license, so vendoring it alongside the zlib/public-domain `lib/ed25519`,
`lib/sha256` isn't a licensing conflict on its face. Not independently verified:
whether Apache 2.0 actually covers the *contents* of that specific binary (WCH
could have bundled a differently-licensed or unlicensed third-party component
inside the compiled archive without it showing in the repo's top-level license)
— worth a final check but not a red flag found so far.
