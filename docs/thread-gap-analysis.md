# SynthPass Discord thread: what's discussed but undocumented

Everything below comes from `docs/synthpass-thread-dump.txt` (the project's Discord
thread, 12/12/25 – present) and is **not** currently covered by `README.md` or
https://pretzel.engineer/synthpass. Where a deep-dive doc already exists in this
`docs/` folder, it's linked instead of repeated.

## 1. Boop protocol (the actual algorithm)

The README says booping happens "by RSSI" but never describes the state machine.
From Pretzel, 12/14/25 and confirmed throughout:

- Every device broadcasts an advertisement (UUID + optional name) every few seconds.
- A nearby device answers with its own UUID + RSSI.
- Once both sides have exchanged a response, they ping each other at a higher rate
  (~a few Hz) to track RSSI over time.
- When both the sent and received RSSI cross a threshold, either side fires the
  "boop" packet — full payload (name/contact info/boop parameters).
- When RSSI drops back below threshold, an "end boop" packet fires.
- Only one full data transfer happens per unique peer device — re-booping the same
  device again doesn't re-collect its message (dedup by UID).
- CRC on received packets was **not** being checked at all until a fix around
  7/9–7/11/26 ("if you receive a message with some funky bytes or another message
  shouting over it, it just gets accepted anyways"). Worth confirming this landed
  cleanly on all paths.
- No defense yet against a "1000 tiny protogens in a trenchcoat" attack — a device
  that spams fake new-peer advertisements to fill a victim's storage. Current
  mitigation is purely physical (you have to be in booping range, or have a
  directional antenna). No rate-limiting has been implemented.

## 2. RSSI ranging / antenna calibration

Not mentioned anywhere in README/site, but central to how boop detection actually
works in practice:

- Boop range detection is fundamentally an RSSI-vs-threshold heuristic, so
  **antenna choice directly changes what counts as a boop**.
- The nanoCH57x board is the ranging baseline. Any other antenna/board needs a
  calibration offset programmed in to match. There's a TX-power/board-type offset
  embedded in the advertised message so receivers can normalize RSSI from
  different antenna setups (an env define like `-e nanoch57x` vs `-e pretzelslab`
  changes this offset, not raw TX power).
- Configurable TX power levels exist in firmware from -25 dBm to +7 dBm
  (`LL_TX_POWER_*` defines) — not documented anywhere as a user-facing setting.
- Calibration itself is currently "guessed," no formal procedure exists yet.
- Real-world antenna findings:
  - Multiple chip antennas tried (Johanson 2450AT42E010B, AANI-CH-0072-T,
    Kinghelm KH-2012-HM1) — several ended up giving bad, hard-to-fix RSSI
    behavior in some layouts (possibly cabling/environment-dependent, not fully
    resolved). No VNA was available to properly tune matching networks.
  - Ground pour/vias under the chip matter a lot for RF return path — one early
    layout was bad enough that "your ground wire on the JST connector is now
    radiating more than the real antenna."
  - Visors and 3D-printed parts barely affect range. Behind an opaque
    display/panel does noticeably reduce range (no hard numbers taken).
  - A PCB stripline/trace antenna variant exists and was believed workable
    ("could delete the whole matching network and just run a 50 ohm stripline"),
    trading board space for cost.
  - RSSI is noisy and orientation-dependent; nearby metal (e.g., a desk frame)
    can cause resonance effects that extend apparent range unpredictably.

## 3. Data/message format details

README says "a couple hundred bytes of plain text" and mentions `ME/BOOP.TXT`,
`ME/PROX.TXT`, `ME/SYNTHPAS.INI`. Missing:

- Filesystem is **FAT12**, built as an in-RAM disk image (boot sector, two FATs,
  root dir) — meaning **8.3 filenames only**.
- Collected messages aren't stored pre-rendered as a flat file; the plan (and at
  least partially implemented) is to store raw message + metadata in flash and
  render `MESSAGES.HTM`/a `.md` file **on the fly** when read over USB, so the
  visible file can be larger than what's actually stored on flash.
- Arbitrary file types are supported for boop payloads, not just text — up to the
  ~241-byte-per-message limit (non-text data gets embedded via markdown image
  tags in the rendered output).
- Text encoding is currently plain ASCII; Unicode/emoji support is an open
  question, not yet decided.
- Practical per-message limit is **241 bytes** (early conversation used "~200
  bytes" as a round number — 241 is the actual current ceiling).
- Storage budget: ~192–200KB of flash for messages ⇒ roughly 800 messages at max
  size. Flash is erased/rewritten in 4KB chunks per modification. Flash write
  endurance was estimated as a non-issue until "approximately 100 million
  protogens" booped.
- No rate-limiting exists yet to prevent one attacker filling all of that storage
  quickly (see the "trenchcoat attack" above).
- Known storage corruption bug: FAT12 image corruption has occurred in testing
  (fixable on Windows only via a PowerShell command + DiskGenius reformat, not a
  plain Windows format). Root causes identified so far: too many files in the
  root directory, and filling storage completely. Not fully resolved/hardened.

## 4. Roadmap items decided/discussed but not published anywhere

- **PAwR** (Bluetooth's actual "Periodic Advertising with Responses" spec) was
  discovered mid-project as an existing standard doing basically what the custom
  iSLER-based protocol does. There's stated intent to potentially replace the
  custom radio implementation with PAwR — a real architectural direction, absent
  from any docs.
- **Protomesh** — a MeshCore-derived mesh networking layer planned for SynthPass:
  - Lifts MeshCore's encryption/identity scheme and a similar (not identical)
    packet format and companion-app protocol.
  - Runs over BLE instead of LoRa: ~100x faster, ~10x shorter range than MeshCore.
  - Planned battery-saving trick: transmit each packet ~10x, only receive ~10% of
    the time — expected to almost 10x battery life vs. always-on RX, since BLE's
    speed budget allows it.
  - Long-term vision: SynthPass's own boop radio protocol may eventually be
    dropped entirely in favor of being "one of many applications built on top of"
    protomesh. This would let SynthPass send/receive encrypted messages through
    the standard MeshCore companion app, publicly or privately, not just do boops.
  - Devices with both a SynthPass radio and a MeshCore (LoRa) radio could bridge
    the protogen mesh into a regional MeshCore mesh (hierarchical nested-mesh
    transport packets already supported by MeshCore's design).
  - Requires a strictly-increasing timestamp per device, meaning devices need to
    keep time across power-off — this is the actual reason a coin-cell battery
    is planned for V2 hardware (RTC-while-off), not general portability.
  - Deep technical detail already captured in [`meshcore_protocol_reference.md`](meshcore_protocol_reference.md).
- **Web-based control panel** (Pretzel, 7/14/26): vision is to make SynthPass
  primarily a USB CDC serial device with a tiny HTML stub on the MSC filesystem.
  Two designs were discussed:
  1. Fully self-contained HTML/JS dashboard living in firmware (rejected as
     probably too big — limits the dashboard to tens of KB).
  2. A tiny HTML stub that fetches the real dashboard/scripts from the internet
     (e.g. GitHub Pages), with config done over serial — the preferred direction,
     since it also unlocks browser-based firmware updates later.
  - A serial bootloader for in-browser firmware updates is planned but not built.
- **OSHW certification** was suggested and tentatively agreed to as a goal once a
  hardware design is finalized and made open-source (not yet done).
- Considering spinning up a **dedicated Discord server** for the project, since
  it has outgrown a single thread (3 hardware + 3 software contributors as of
  the most recent messages) — a community/process note, not firmware, but
  relevant if anyone's looking for "where discussion happens now."

## 5. Hardware facts missing from README/site

- **CH570 vs CH572**: pin-for-pin compatible, both 32-bit RISC-V. CH572 costs
  more (~$0.32–0.36 vs. CH570's ~10¢ cheaper) purely because of BLE licensing
  fees; CH570 lacks BLE. The project is built primarily against the CH572 but
  people have used CH570 boards during bring-up.
- **Crystal requirement**: a 32MHz crystal (or MEMS oscillator marketed as one)
  is *required*, not optional — the CH57x has no internal high-speed oscillator,
  and I2C/wireless both need it.
- **1.5kΩ USB pull-up resistor**, not 5.1kΩ — a hardware bug present on multiple
  board revisions (per the nanoCH57x maker's correction), planned to be fixed in
  SynthPass hardware V2.
- **USB-C CC resistors**: R1 boards lacked separate CC pull-down resistors, which
  can cause issues with e-marked USB-C cables specifically (this is in addition
  to, not the same as, the "R1 pinout reversed" issue already on the website).
- Ground vias/pour underneath the CH57x are necessary for a clean RF return path
  — omitting them caused a board's ground wire to radiate more than its antenna.
- LDO question left open: whether to use the CH572's internal LDO to accept 5V on
  the JST 3.3V line directly, or add an external LDO (TI TLV742P was evaluated:
  ~14–17¢, small QFN package). Decision affects whether the SH connector can
  safely be fed 5V. Full component-level writeup in
  [`power_supply_components.md`](power_supply_components.md).
- Multiple independent hardware variants exist and are treated as acceptable, not
  as fragmentation: AxionQc's micro board (10x16mm, later revisions), Pretzel's
  PCB-trace-antenna design, and a community 2-layer variant by Arc (15.8x16.8mm,
  drops the JST connector, smaller crystal, 2-layer to save cost). The
  `nanoch57x` dev board is explicitly meant to remain a permanently supported
  DIY-friendly option, not just a bring-up board.
- Board cost breakdown: roughly $1–2 in components per board; the antenna is
  typically the single most expensive part (~50–60¢, i.e. more than the CH572
  itself at ~32–36¢). JLC assembly quotes ranged $55–89 for 20–30 boards, ~$274
  for 200 (tariff-inflated), pushing the team toward home assembly for volume.
- WS2812B addressable LED support is implemented as the status LED. V6 WS2812
  chips need a >280µs reset pulse (longer than commonly assumed ~68µs), driven
  via the PWM/DMA-SPI driver in `ch32fun`'s `ws2812b_dma_spi_led_driver.h`.
  Planned (not yet finalized) LED behavior: solid color when idle, blink on
  transmit, pulse a different color on boop.
- 3D models were published to the GitHub repo as of 7/11/26 — not referenced in
  README.
- A Wireshark dissector for SynthPass packets exists (written ~2/8/26) — useful
  for anyone debugging the radio protocol, not mentioned anywhere in docs.

## 6. Power / battery

Not mentioned in README/site at all (project is currently framed as USB/fursuit
powered only):

- Measured current draw: ~9–12mA typical active (radio + USB), dropping to
  ~9mA at 16MHz core clock with PLL and USB disabled.
- Disabling unused peripheral clocks (UART, comparator, timer, PWM, SPI) saves
  ~600µA.
- AES block can likely be safely disabled since SynthPass only sends unencrypted
  advertisements today (protomesh will presumably need it back).
- Target for a future battery-powered mode: ~500µA average via periodic RX +
  repeated broadcast duty cycling.
- Battery life estimates: ~20 hours on a CR2032 (240mAh) vs. ~4 hours on a
  rechargeable LIR2032 (45mAh) at current draw levels — CR2032 (non-rechargeable)
  is the better fit given consumption.
- Proposed trick: run USB VBUS through a diode into a spare GPIO as a "USB
  plugged in" detect line, so the MCU can enable the PLL/USB clock only when
  actually plugged in and drop to a low-power clock config otherwise.
- V2 hardware is planned to add a coin-cell holder/connector — driven by
  protomesh's need for a timestamp that survives power-off, not just "nice to
  have" battery operation.
- Full BOM/IC-level treatment already exists in
  [`power_supply_components.md`](power_supply_components.md).

## 7. Firmware/toolchain internals and known gotchas

None of this is in README, which only covers `pio run -e <target> [-t upload]`:

- Flash write/erase code **must run from RAM**, not flash — requires a
  `__HIGH_CODE` annotation. Getting this wrong (or not knowing about it) was a
  multi-hour debugging trap for at least one contributor trying to implement
  persistence.
- The CH57x RISC-V core has vendor custom instructions, including a
  memcpy-equivalent instruction and compressed load/store opcodes that GCC
  won't emit automatically — a PR exists redefining `memcpy` to use these, plus
  discussion of hand-writing `__HIGH_CODE` functions in assembly for size wins
  (intentionally deprioritized for now — "a lot is going to change with this
  firmware soon").
- Persistence design: message data is written to flash (not left in RAM), and
  firmware re-reads flash on boot to reconstruct pointers to stored messages.
- A "factory setting" can leave a freshly-flashed board apparently bricked
  (flashes fine, application never runs); fix is `minichlink -P` then
  re-flashing. This bit multiple people independently and isn't documented.
- Observed flakiness: firmware sometimes boots successfully only ~10–20% of the
  time after flashing, requiring repeated unplug/replug. At least one instance
  traced to a WS2812 driver bug, but general flakiness has been reported
  separately and isn't considered fully solved.
- Debug workflow is USB CDC serial print-based, **not** the WCH-Link debug
  probe — the debug pins are shared with USB D+/D-, so the probe is unusable
  once USB is enabled in firmware. WCH-Link is only useful for
  non-USB-enabled experimentation.
- Linux: udev rules for `minichlink`/USB access are often insufficient even with
  correct group membership (`plugdev`, `dialout`); at least one contributor
  never got non-sudo access working (Arch-specific quirk, unresolved).
- Windows: requires **Zadig** to install a WinUSB driver on the bootloader
  device; the BOOT button must be held down continuously through the *entire*
  flash process, not just at plug-in, or the device becomes unflashable until
  replugged with the button held again.
- Expected-good `lsusb -t` output (three interfaces under one device — CDC
  control, CDC data, Mass Storage) and expected `dmesg` output on successful
  enumeration were shared as debugging references but never written down
  anywhere permanent.
- Editing `SYNTHPAS.INI` should not require re-uploading firmware — only code
  changes need a re-flash — but this hasn't been verified on Windows and file
  handling there is described as "weird."
- The project moved from a Makefile-based build to PlatformIO (merged 2/8/26);
  the VSCode PlatformIO extension has been reported completely broken for at
  least one contributor (CLI works fine) — worth a troubleshooting note.

## 8. Vendor lock-in stance (explicit design discussion, undocumented)

A thread from 1/21/26 explicitly discusses and settles a question that isn't
answered anywhere in the docs: **iSLER currently only runs on WCH CH5xx chips.**
Porting to another vendor would require reverse-engineering their radio hardware.
The stated position is pragmatic — get something working and nail the user
experience first, worry about cross-vendor portability later — but this
project-level design decision (and its rationale) isn't written down anywhere a
future contributor would see it.

## 9. Not worth a doc, but worth knowing

- Name history: the project was called "UBoop" (a pun on U-Boot) before
  settling on "SynthPass" — in case old references to UBoop turn up anywhere.
- The `AxionQc` hardware target in `platformio.ini` refers to a real independent
  contributor's board design, not a placeholder.
