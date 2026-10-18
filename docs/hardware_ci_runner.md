# Hardware CI test runner — setup work needed

Goal: turn `jack-framework` (10.0.0.159), the Manjaro laptop currently used
for manual board bring-up, into an unattended runner that flashes and
smoke-tests real SynthPass hardware on push/PR, not just `native` unit tests.

This documents what's already in place, what's missing, and open questions.
It's a scoping doc, not a how-to — nothing here has been implemented yet.

## What's already in place

- **Flashing tool**: `flash_loop.sh` watches USB (via `/sys/bus/usb/devices`)
  for either VID:PID (`1209:d035` app / `1a86:55e0` WCH ISP bootloader),
  auto-enters the bootloader over CDC if needed, flashes with `minichlink`,
  and appends a row (timestamp, env, bus path, chip UID, derived SynthPass
  UID) to `flashed_devices.tsv`.
- **USB permissions**: `jack` is already in `plugdev` and `dialout`, so
  `minichlink` and serial access work without sudo. No udev rules needed for
  the flashing path itself.
- **Repo checkout**: `~/git/SynthPassFw` on the remote host tracks `origin`
  and is kept in sync manually today (`git pull` + `pio run`).
- **Native unit tests exist** (`test/test_packet`, `test/test_identity`,
  `env:native` in `platformio.ini`) but don't need hardware at all — these
  should run in normal (non-hardware) CI, e.g. GitHub-hosted runners, and are
  out of scope for this doc except as a reminder they're a separate track.

## Gaps to close

### 1. No CI runner installed
There's no GitHub Actions self-hosted runner, or any other CI agent, on the
machine — confirmed nothing under `~/actions-runner` and no matching
systemd units. Needs: install and register a self-hosted runner (or
equivalent — Buildkite agent, etc.) as a persistent service (`systemctl
--user enable` or a system unit), scoped with a label like
`self-hosted, hardware, synthpass` so workflows opt in explicitly rather than
this runner picking up arbitrary jobs.

### 2. Machine goes to sleep
`/etc/systemd/logind.conf` has no overrides — default lid-close/idle suspend
behavior is active. This session saw the host drop off the network twice
("No route to host") consistent with suspend. A CI runner needs the host up
and USB-alive continuously. Needs: `HandleLidSwitch=ignore`,
`HandleLidSwitchExternalPower=ignore`, disable idle suspend (`systemctl mask
sleep.target suspend.target`), and confirm AC power / no battery-driven
suspend policy fights it. This requires root — `sudo` on this host currently
prompts for a password (no NOPASSWD entry), so either a one-time interactive
sudo session or an explicit sudoers entry is needed before this can be
scripted.

### 3. Board fixture / stable board identity
`flash_loop.sh` today assumes a human plugs in one board, watches it flash,
unplugs, plugs the next — bus path is only used to dedupe within a session,
not to name a board. For CI we want fixed, known-good hardware permanently
wired to specific hub ports (e.g. "port 3-3.1 is board A"), so a test run can
target/report on a specific physical unit and results are reproducible
across runs. Needs: pick N boards to dedicate as the permanent CI fixture,
wire them to fixed hub ports, and record the bus-path → board-identity
mapping (the existing `part_uuid`/`synthpass_uid` columns in the ledger are
already suitable keys for this once a mapping exists).

### 4. No unattended recovery from a bricked board
This is the sharp edge: the exact failure that bricked 5 boards this session
(radio ISR firing mid-`jump_isprom()`, corrupting the ROM bootloader — see
[[ch572-bootloader-jump-brick]]) is a firmware bug, which means a CI job
flashing an in-progress/broken branch can reproduce it. When that happens
today, the board goes fully dark on USB and the only recovery is holding the
BOOT button while re-plugging — physical intervention. An unattended runner
needs a software-controlled way to power-cycle individual hub ports (into
the bootloader's hardware boot-button state is not achievable by power alone,
so this really needs a way to force BOOT-pin-low across a power cycle, e.g.
via a controllable relay/GPIO on the boot line, not just hub port power).
Current state: `uhubctl` isn't installed, and the hub in the chain
(`05e3:0610`, Genesys Logic 4-port, two chained to make the 7 ports) hasn't
been confirmed to support per-port power switching — that's hit or miss on
cheap hubs and needs to be tested. If it doesn't support per-port switching,
a small board (Pi Pico + relay/FET, or a USB-controlled relay board) driving
BOOT + reset lines directly is the fallback. Without *some* answer here, one
bad firmware build silently takes a fixture board offline until a person
walks over — that's the actual blocker to "unattended."

### 5. `flash_loop.sh` is interactive, not CI-shaped
It's a `while true` TUI loop meant for a human watching a terminal — colored
frames, "waiting for a board to plug in," Ctrl-C to quit. A CI step needs a
non-interactive invocation that: targets one specific known board (not "next
thing plugged in"), flashes it, runs a pass/fail check, and exits with a
real status code plus machine-readable output (JSON line or similar) rather
than a TUI redraw. This is probably a new script that reuses `flash_loop.sh`'s
USB/minichlink helper functions rather than a rewrite of the loop itself.

### 6. No on-target test beyond "did it flash"
Today "success" means `minichlink` returned 0 and the board re-enumerated as
`1209:d035`. There's no actual functional check — e.g., confirm the CDC port
responds, confirm the radio comes up and broadcasts, or (more valuable)
place two fixture boards near each other and confirm they detect each other
and exchange a boop/PROX message. That last one is the real integration test
and doesn't exist in any form yet — it needs a serial-side protocol to ask a
running board "did you see a peer / did a boop happen," which the firmware
doesn't currently expose (the CDC link is currently a bare debug/bootloader
channel, not a query protocol).

### 7. Concurrency / locking
We already hit a real incident this session where a flaky SSH connection
appeared to spawn duplicate `flash_loop.sh` instances against the same
machine (resolved itself, ledger stayed intact, but the race — two
`minichlink` invocations against the same bus path — was real). A CI runner
needs to guarantee only one flash/test operation touches the USB bus at a
time, e.g. a simple flock around the whole job step.

### 8. Self-hosted runner security posture
The repo (`github.com/JackToaster/SynthPass`) is public. GitHub's own
guidance is explicit that self-hosted runners should not be reachable from
`pull_request` on public repos, since a fork's workflow YAML runs arbitrary
code on the runner. Needs: scope the hardware-runner workflow to `push`
(protected branches only) and/or `workflow_dispatch`/label-gated
`pull_request_target` with maintainer approval, never plain `pull_request`.

### 9. Network stability
The host is reachable at 10.0.0.159 today but that's presumably DHCP —
worth a static reservation on the router/DHCP server so the runner
registration and any hardcoded CI config don't break on lease renewal.

## Suggested order of work
1. Confirm hub per-port power switching (`uhubctl`) — this determines
   whether #4's recovery path is cheap (software) or needs new hardware.
2. Fix sleep/suspend (#2) — cheapest fix, unblocks "always reachable."
3. Build the non-interactive single-board flash+status script (#5), reusing
   `flash_loop.sh` internals.
4. Wire up the fixture (#3) once boards are dedicated and #5 exists.
5. Install and register the runner (#1), scoped per #8, with locking (#7).
6. On-target functional test (#6) is the highest-value but highest-effort
   item — reasonable to land basic flash-and-boot CI first, and treat the
   peer-detection test as a follow-up.
