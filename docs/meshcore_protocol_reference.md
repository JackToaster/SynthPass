# MeshCore Protocol Reference (for a SynthPass / CH572-CH583 port)

This document explains the three parts of MeshCore that matter if we want SynthPass
hardware to speak the same protocol as real MeshCore nodes and stay compatible with
the official MeshCore companion phone app: the **packet format**, the **identity and
encryption scheme**, and the **routing algorithms**.

Each section ends with a verdict:

- **LIFT** — copy the logic (or a straight C translation of it) as-is. Getting this
  wrong breaks compatibility.
- **REWRITE** — the *idea* is worth keeping, but the implementation must change
  (usually because it assumes LoRa, or assumes more RAM/flash than we have).
- **DROP** — not needed for a BLE-based, possibly single-node or small-mesh device.

A full checklist (with current status) is in [Section 4](#4-summary-checklist); milestones for finishing the
implementation and testing it on hardware are in [Section 5](#5-implementation-status-and-milestones).

All source references are to files in `~/git/MeshCore` (`src/Packet.h`, `src/Utils.cpp`,
`src/Identity.cpp`, `src/Mesh.cpp`, `src/Dispatcher.cpp`) unless noted.

---

## 1. Packet format

Every piece of data that moves through a MeshCore mesh — a chat message, an ACK, a
node announcing itself — is wrapped in one **Packet**. A Packet is a small, fixed-shape
envelope: a header byte, an optional routing path, and a payload.

### 1.1 The envelope, byte by byte

When a Packet is sent over the radio, it is packed into raw bytes in this order
(`Dispatcher::checkSend`, `Dispatcher::tryParsePacket`):

| Field | Size | Present when |
|---|---|---|
| `header` | 1 byte | always |
| `transport_codes` | 4 bytes (2×uint16) | only for "transport" route types (see 1.2) |
| `path_len` | 1 byte | always |
| `path` | 0–64 bytes | length given by `path_len` |
| `payload` | remaining bytes | always (no length field — see below) |

**Important quirk:** the payload has no length byte of its own. Its length is just
"whatever bytes are left in the received frame." This works because a radio receive
event naturally has a clean start/end (one LoRa packet = one MeshCore packet). A BLE
advertising PDU has the same property (its own length byte already tells you where
the frame ends), so this framing style carries over cleanly.

Size limits (`MeshCore.h`):

| Constant | Value | Meaning |
|---|---|---|
| `MAX_PACKET_PAYLOAD` | 184 bytes | biggest payload a Packet can carry |
| `MAX_PATH_SIZE` | 64 bytes | biggest routing path |
| `MAX_TRANS_UNIT` | 255 bytes | biggest whole packet (header + path + payload) |
| `MAX_HASH_SIZE` | 8 bytes | biggest single hash used in routing (see 1.3) |

### 1.2 The header byte

One byte, split into three small fields:

```
bit:    7 6 5 4 3 2 1 0
        [ver ][--type--][rt]
```

- **`rt`** (bits 0–1): **route type** — how this packet should be delivered. Four values:
  - `FLOOD` (0x01) — broadcast to everyone; each node that gets it may re-broadcast it.
  - `DIRECT` (0x02) — follow a specific path (a list of hops) already known.
  - `TRANSPORT_FLOOD` / `TRANSPORT_DIRECT` — same as above, but with 4 extra
    "transport code" bytes tacked on, used by dedicated infrastructure repeaters to
    tunnel traffic. Not something an end device needs to generate.
- **`type`** (bits 2–5): **payload type** — what kind of thing is inside (see 1.4).
- **`ver`** (bits 6–7): **format version**. Only `0` ("V1") is defined right now.

### 1.3 The path field

`path_len` is not a plain byte count — it's packed:

- Top 2 bits: "hash size mode" (how many bytes per hop-hash: 1, 2, 3, or 4 bytes).
- Bottom 6 bits: how many hops are in the path (0–63).

Each hop in `path` is a short hash (a few leading bytes of that node's public key —
just enough to identify it locally, not the whole 32-byte key). A path is built up
hop-by-hop as a flood packet travels, or supplied directly once a route is known.

### 1.4 Payload types

The `type` field says what's inside the payload. Plain-English meaning of each:

| Value | Name | Plain English |
|---|---|---|
| 0x00 | `REQ` | A request to another node (encrypted). |
| 0x01 | `RESPONSE` | Reply to a `REQ`. |
| 0x02 | `TXT_MSG` | A normal 1-to-1 chat message (encrypted). |
| 0x03 | `ACK` | "I got your message" confirmation. |
| 0x04 | `ADVERT` | "Here's who I am" — a node announcing its identity. |
| 0x05 | `GRP_TXT` | A group/channel chat message (encrypted with a shared channel key, not per-recipient). |
| 0x06 | `GRP_DATA` | Same as `GRP_TXT` but raw binary instead of text. |
| 0x07 | `ANON_REQ` | A request from someone who doesn't have an established contact relationship yet. |
| 0x08 | `PATH` | "Here's the route back to me" — sent so the other side can reply directly instead of flooding. |
| 0x09 | `TRACE` | Debug/diagnostic: trace a route and collect signal-quality info per hop. |
| 0x0A | `MULTIPART` | This packet is one piece of a message split across several packets. |
| 0x0B | `CONTROL` | Low-level control/discovery traffic. |
| 0x0F | `RAW_CUSTOM` | Anything else — app-defined, no built-in meaning. |

### Verdict

| Piece | Verdict | Why |
|---|---|---|
| Header byte layout (route/type/version bits) | **LIFT** | Trivial to implement, and this is the one thing that must match byte-for-byte if any two independently-built nodes are ever to understand each other's packets. |
| Path encoding (`path_len` packing, hop hashes) | **LIFT** | Same reasoning — small, mechanical, load-bearing for interop. |
| "No length byte, payload = remainder" framing | **LIFT** | Free — BLE advertising PDUs already give you a clean frame boundary, same as LoRa does. |
| Payload type table | **LIFT** (the ones we use) | We don't have to implement every type (e.g. `TRACE`, `CONTROL` can wait), but the ones we do implement must use the correct numeric value and byte layout, or the companion app/other nodes will misparse them. |
| `MAX_*` size constants | **LIFT** | Just numbers — keep them so packets stay a size any MeshCore-speaking radio can carry. |

---

## 2. Identity and encryption

### 2.1 Identity: one keypair does two jobs

Every node has one **Ed25519** public/private keypair (`Identity.h`, `Identity.cpp`).
Ed25519 is a well-known, well-tested type of public-key cryptography. This single
keypair is used for two different things:

1. **Signing** — proving "this advert/message really came from me" (`ed25519_sign` /
   `ed25519_verify`). The node's advertisement packet (`PAYLOAD_TYPE_ADVERT`) is
   signed so other nodes (and the companion app) can trust the identity is real, not
   spoofed.
2. **Key exchange** — deriving a **shared secret** with another node, so the two of
   them (and only them) can encrypt messages to each other (`ed25519_key_exchange`).
   This reuses the same keypair math (Ed25519 keys can double as Curve25519/X25519
   keys), so there's no second keypair to generate or manage.

Key sizes: public key 32 bytes, private key 64 bytes (`MeshCore.h`).

The node's public key **is** its identity — the companion app shows it, exports it,
puts it in QR codes. Any other MeshCore user who has your public key can compute the
same shared secret with you (their private key + your public key = their private key
+ your public key, by the math of Diffie-Hellman-style exchange — that's the whole
point).

### 2.2 Turning a shared secret into message encryption

The 32-byte shared secret from step 2.1 is used as follows (`Utils.cpp`):

1. **Encrypt**: AES-128 in ECB mode (a standard block cipher), keyed with the
   **first 16 bytes** of the shared secret. The plaintext is padded with zero bytes
   up to a multiple of the 16-byte block size.
2. **Authenticate**: HMAC-SHA256 is computed over the *ciphertext* (not the
   plaintext), keyed with the **full 32-byte** shared secret, and truncated to just
   **2 bytes** (`CIPHER_MAC_SIZE`). This is prepended to the ciphertext.

This order — encrypt, then compute a MAC over the ciphertext — is called
**"encrypt-then-MAC."** It's a standard, safe way to combine encryption (keeps the
message secret) with authentication (proves it wasn't tampered with, and that it
was really encrypted by someone who has the shared secret). On decrypt, the MAC is
checked *before* decrypting; if it doesn't match, the packet is silently dropped.

In plain English: **only someone who can compute the same shared secret can produce
a MAC that matches**, so an attacker can't forge or corrupt messages without it being
detected. The MAC is only 2 bytes, which is short for cryptographic tags (normal
practice is 16 bytes) — this is a deliberate MeshCore tradeoff to save airtime on a
slow radio channel, accepting a higher (but still low) chance a forged message
sneaks through by luck.

**Group/channel messages** use the same encrypt-then-MAC scheme, but the "shared
secret" is a fixed 16-byte key everyone on that channel already knows, instead of a
per-pair Diffie-Hellman result. Two channel types matter:
- The **public channel** uses a fixed, published key
  (`8b3387e9c5cdea6ac9e5edbaa115cd72`) — anyone can decrypt it, it's really just
  obfuscation, not real privacy.
- **Hashtag channels** (e.g. `#test`) derive their key as the first 16 bytes of
  `sha256("#test")` — same idea, key is guessable if you know/guess the channel name.
- **Private channels** use a random 16-byte secret shared out-of-band, and are
  actually private.

### 2.3 Hashing: SHA-256, used two different ways

SHA-256 (a standard cryptographic hash function) shows up in two unrelated places:

1. **Packet hash** (`Packet::calculatePacketHash`) — a hash of a packet's contents,
   used to recognize "I've already seen this exact packet" so it isn't
   re-processed or re-flooded twice (see Section 3).
2. **ACK hash** — when node A sends a text message, it independently computes
   `sha256(timestamp + attempt_number + text, senders_public_key)` and remembers the
   first 4 bytes as the "expected ACK." When node B receives and successfully
   decrypts that message, it computes the *exact same hash* from the bytes it
   decrypted, and sends those 4 bytes back as the ACK. Node A recognizes its own
   expected value in the reply. **There is no message ID or sequence number** — the
   ACK is just proof "I decrypted the same bytes you sent," which also means
   receiving the same ACK twice (via two different paths) is harmless, since it's
   just being compared, not counted.

### Verdict

| Piece | Verdict | Why |
|---|---|---|
| Ed25519 keypair (sign + verify + key exchange) | **LIFT** | This is the core of what "identity" means in MeshCore. The companion app displays/exports this key. Fake or non-standard keys break compatibility the moment any real verification happens (by the app, another node, or later versions of our own firmware). Use the same portable C `lib/ed25519` MeshCore uses — no reason to reinvent it. |
| AES-128-ECB + HMAC-SHA256(2-byte) encrypt-then-MAC | **LIFT** | Must match exactly, or encrypted traffic (DMs, channel chat) is unreadable by anyone else — including a second unit of our own hardware. |
| Public/hashtag channel key derivation | **LIFT** | Needed if we want to show up on the same default channels real MeshCore users are on. Cheap — it's just a hash or a constant. |
| ACK-hash formula | **LIFT** | Small (one `sha256` call), and it's the entire reliability mechanism the companion app's "delivered" indicator depends on. |
| Packet-hash-based dedup | **LIFT** (the hash itself) — routing behavior is a separate question, see Section 3 | The hash function is trivial; what a node *does* with it (relay decisions) is where the real complexity/LoRa-specific tuning lives. |

---

## 3. Routing algorithms

MeshCore has two "ways to deliver a packet" — **flood** and **direct** — plus a
scheduler (the **Dispatcher**) that decides *when* the radio actually transmits.

### 3.1 Flood routing: "shout and let it propagate"

A flood packet has no predetermined path. Every node that hears it may decide to
re-broadcast it, so it spreads outward hop by hop until it's been everywhere it can
reach. Two problems this creates, and how MeshCore solves them:

**Problem 1 — duplicate storms.** If every node that hears a flood packet
re-broadcasts immediately, many nodes transmit at once and collide.
**Solution — signal-quality-weighted delay.** Before deciding whether to relay a
flood packet, a node computes a **delay** based on how well it received the packet
(`Dispatcher::calcRxDelay`): a clean, strong reception gets a short delay; a weak,
marginal reception gets a long delay (up to 32 seconds). A node only actually
processes/relays the packet once its delay timer expires — and if, in the meantime,
it hears the *same* packet already relayed by someone else (matched by packet hash,
"dedup," see 2.3), it just drops its own copy instead of also relaying. In effect:
whichever node heard the packet best gets to relay it first, and everyone else backs
off. On top of that, once a node *does* decide to relay, it adds a second random
jitter (`Mesh::getRetransmitDelay`) so even similarly-scored nodes don't transmit in
lockstep.

**Problem 2 — knowing the way back.** A flood packet doesn't carry a fixed
destination path — but every hop it passes through appends itself to a `path` field
(Section 1.3), so by the time it arrives, the path field is a breadcrumb trail back
to the sender. The receiver can reply along that exact trail using **direct** routing
instead of flooding another reply outward.

**Deduplication.** Every node remembers the hashes of packets it has already
processed (`wasSeen`/`markSeen`). A flood packet that arrives twice (common — it
often arrives via more than one path) is recognized and dropped the second time,
not reprocessed or re-relayed.

### 3.2 Direct routing: "follow this exact list of hops"

Once a path is known (from a flood packet's breadcrumb trail, or exchanged
explicitly), a packet can be sent `DIRECT`, carrying the path with it. Each
intermediate node checks: "am I the next unconsumed hop in this path?" — if yes, it
strips itself off and forwards; if no, it silently drops the packet (it's not this
node's job to relay it). This is far cheaper than flooding (no signal-quality
jitter needed — there's exactly one intended relay at each hop, not a competition).

### 3.3 The Dispatcher: when to actually transmit

Everything above is about *what* to send. The **Dispatcher** decides *when* the radio
is allowed to key up. Four LoRa-specific mechanisms:

- **Duty-cycle budget** — a token bucket that limits how much airtime the radio is
  allowed to use per hour, refilled continuously, spent on every transmit. This
  exists because LoRa radios in most of the world are legally capped to a small
  percentage of airtime (e.g. 1% in the EU).
- **CAD (Clear Channel Assessment)** — before transmitting, check if the radio is
  mid-receive of something else, and back off if so (with a timeout so a stuck
  radio doesn't block transmit forever).
- **Noise-floor / AGC calibration** — periodic housekeeping specific to the LoRa
  radio hardware, to keep its receive sensitivity well-tuned.
- **`packetScore`** — a quality estimate (0–1) from the *received* signal, used to
  drive the flood relay delay in 3.1. On LoRa this is computed from SNR (signal-to-noise
  ratio) relative to a threshold that depends on the LoRa spreading factor in use.

### 3.4 End-to-end reliability (recap from Section 2.3)

Only **direct chat/text messages between two known identities** get a
delivery guarantee of any kind, and even that's minimal: the sender computes a
timeout (bigger for flood delivery, scaled by hop count for direct delivery) and, if
no ACK arrives in that window, just tells the application layer "this timed out" —
**MeshCore itself never automatically retries.** The connected phone app decides
whether and when to resend. Flood-relay hops themselves are never acknowledged or
retried at all — the mesh is best-effort at every hop except the final
sender-to-recipient round trip.

### Verdict

| Piece | Verdict | Why |
|---|---|---|
| Flood route type + path-breadcrumb accumulation | **LIFT** (if any multi-node relay is wanted) | This is the actual "mesh" in MeshCore, and it's how a reply path gets established without a prior handshake. Needed the moment more than one hop matters. |
| Direct route type + next-hop path following | **LIFT** (same condition) | Cheap once flood/path exists, and it's what makes replies efficient instead of re-flooding everything. |
| Packet-hash dedup (`wasSeen`/`markSeen`) | **LIFT** | Simple to implement (a small recently-seen-hash cache) and necessary the moment flood routing exists at all, or the same packet gets processed/relayed repeatedly. |
| Signal-quality-weighted relay delay (`calcRxDelay`) | **REWRITE** | The *idea* (let the best-positioned node relay first, make others back off) is sound and worth keeping for any BLE flood scheme too — but the formula is tuned around LoRa SNR/spreading-factor thresholds. Needs a new formula based on BLE RSSI instead. |
| Retransmit jitter (`getRetransmitDelay`) | **REWRITE** | Same idea, same reason — recompute the timing constants for BLE's much shorter airtimes instead of LoRa's. |
| Duty-cycle token bucket | **DROP** | This exists purely for LoRa regulatory airtime limits. BLE advertising has no equivalent regulatory duty-cycle constraint at these power levels. |
| CAD (Clear Channel Assessment) | **DROP or trivial replacement** | LoRa-radio-specific concept (checking a dedicated "is the radio mid-receive" hardware flag). A BLE-based design would use something much simpler if anything — e.g. a brief RSSI listen, similar to what SynthPass's own boop-detection already does. |
| Noise-floor / AGC calibration | **DROP** | LoRa radio hardware housekeeping; doesn't apply to a fixed-PHY BLE radio. |
| `packetScore` (SNR/spreading-factor based) | **REWRITE** | Needed as an input to the relay-delay rewrite above, but must be recomputed from RSSI (what a BLE radio actually reports) rather than LoRa SNR thresholds. |
| End-to-end ACK + app-driven timeout/retry | **LIFT** | This is Section 2.3's ACK-hash scheme plus a timeout calculation — small, radio-agnostic, and it's the entire reliability contract the companion app expects. |
| Full `BaseChatMesh` feature surface (ACL, telemetry, `TRACE` route diagnostics, transport-tunnel codes) | **DROP** (unless specifically needed) | None of this affects whether basic chat/channel/contact functionality works or is companion-app-compatible. Add only if a specific feature is wanted later. |

---

## 4. Summary checklist

| # | Component | Verdict | Section | Status (2026-09-27) |
|---|---|---|---|---|
| 1 | Header byte (route/type/version bits) | **LIFT** | 1.2 | ✅ `pack_header`/`unpack_header`, tested |
| 2 | Path field encoding | **LIFT** | 1.3 | ✅ `pack_pathlen`/`unpack_pathlen`; validation is a TODO (M1) |
| 3 | "No length byte" payload framing | **LIFT** (free) | 1.1 | ✅ in `pack_packet`/`unpack_packet`; **no RX bounds checks** (M1) |
| 4 | Payload type byte layouts (the ones we use) | **LIFT** | 1.4 | ⬜ constants only; no ADVERT/TXT_MSG/ACK/GRP_TXT/PATH layouts yet (M1, M5) |
| 5 | `MAX_*` size constants | **LIFT** | 1.1 | ✅ `packet.h`/`identity.h` |
| 6 | Ed25519 identity (sign/verify/key-exchange) | **LIFT** | 2.1 | 🟨 sign/verify/key-exchange/key validation tested; no keygen or key storage (M1, M3). Code region needs expanding to fit (M2) |
| 7 | AES-128-ECB + HMAC-SHA256 encrypt-then-MAC | **LIFT** | 2.2 | ✅ `encryptThenMAC`/`MACThenDecrypt` tested vs reference; needs length hardening (M1) |
| 8 | Public/hashtag channel key derivation | **LIFT** | 2.2 | 🟨 `derive_channel_key` tested; public key constant is unusable as written, and the 16→32 byte HMAC key padding isn't handled (M1) |
| 9 | ACK-hash formula | **LIFT** | 2.3 | ⬜ (M5) |
| 10 | Packet-hash function (for dedup) | **LIFT** | 2.3 | ✅ `packet_calculate_hash`, matches MeshCore vectors incl. TRACE |
| 11 | Flood route type + path breadcrumbing | **LIFT** (if multi-node) | 3.1 | ⬜ (M6) |
| 12 | Direct route type + next-hop following | **LIFT** (if multi-node) | 3.2 | ⬜ (M6) |
| 13 | Dedup cache (`wasSeen`/`markSeen`) | **LIFT** (if multi-node) | 3.1 | ⬜ (M6) |
| 14 | Signal-quality relay delay | **REWRITE** (RSSI, not SNR/SF) | 3.1 / 3.3 | ⬜ (M6) |
| 15 | Retransmit jitter | **REWRITE** (BLE airtime constants) | 3.1 | ⬜ (M6) |
| 16 | `packetScore` | **REWRITE** (RSSI-based) | 3.3 | ⬜ (M6) |
| 17 | Duty-cycle token bucket | **DROP** | 3.3 | — dropped |
| 18 | CAD | **DROP** / trivial replacement | 3.3 | — dropped (M4 may add a short RSSI listen-before-talk) |
| 19 | Noise-floor / AGC calibration | **DROP** | 3.3 | — dropped |
| 20 | End-to-end ACK + timeout (app-driven retry) | **LIFT** | 3.4 | ⬜ (M5) |
| 21 | ACL / telemetry / trace-route / transport-tunnel features | **DROP** (unless needed) | 3.4 | — dropped |

**Rule of thumb:** everything that defines what a packet *means* to another party —
its wire format, its crypto, its identity, its ACK contract — should be lifted
verbatim, because that's the actual definition of "compatible." Everything that's
about *scheduling radio transmissions on LoRa specifically* should be rewritten or
dropped, because none of it applies to a BLE-based radio, and none of it is visible
to the companion app or to another node's *interpretation* of a received packet —
only to how efficiently/politely the airtime is used getting there.

---

## 5. Implementation status and milestones

*Updated 2026-09-27, based on the `mesh` branch at `efed1c9`.*

### 5.1 Where things stand

**Done, and tested on the host** (`pio test -e native`: 29/29 passing):

| Area | Files | Tests |
|---|---|---|
| Packet envelope: header, `path_len` packing, pack/unpack, packet hash | `src/packet.{c,h}` | `test/test_packet` (5), including hash vectors that match MeshCore for normal and `TRACE` packets |
| Identity: Ed25519 sign/verify, X25519 key exchange, private key validation (ported from MeshCore's `validatePrivateKey`) | `src/identity.{c,h}`, vendored `lib/ed25519` | `test/test_identity` (6) |
| Crypto: AES-128-ECB, HMAC-SHA256, encrypt-then-MAC, hashtag channel key derivation | `src/utils.{c,h}`, vendored `lib/aes`, `lib/sha256` | `test/test_utils` (18), checked against reference outputs |

**Not started:** payload layouts (ADVERT, TXT_MSG, ACK, GRP_TXT, PATH), key generation and storage, anything
on the radio, dedup and routing, contacts and channels storage, host/companion interface. None of the
mesh code is called from the firmware yet. `SynthPass.c` and `radio.c` still run only the original boop
protocol.

**Problems found during this review.** Each one is assigned to a milestone below.

1. **The CH572 firmware doesn't build on this branch.** `e00d0c2` deleted `src/config.c`, but
   `SynthPass.c`, `radio.c` and `usb.c` still call `config_init`/`config_get`/`config_apply_text`/
   `config_rendered*`, so the build fails at link time. Config is going to be reworked, so rather than
   restoring the old file, M0 stubs out or removes those calls until the rework (M8). → M0
2. **The `nanoch57x`, `pretzelslab` and `axionqc` envs are broken.** When `[env]` became
   `[ch572_common]`, only `genericCH572Q` got `extends = ch572_common`. The other three now fail with
   `UndefinedEnvPlatformError`. The branch also dropped the `symlink://../ch32fun` framework override, so
   builds now use the pinned package. That is probably fine (a scratch build with `config.c` restored
   links), but the change should be deliberate. → M0
3. **The crypto needs a larger code region. This is a task, not a blocker.** The linker script gives
   code **48 KB** (the other 192 KB is reserved for the message store). The current firmware, with
   `config.c` restored, uses **~16.6 KB flash and ~5.9 KB of 12 KB RAM**, which leaves ~31 KB free.
   Compiling the mesh and crypto sources on their own (`-Os`, rv32imac) gives:

   | Object | `.text` |
   |---|---|
   | `ed25519/ge.o` (mostly the `precomp_data.h` base-point table) | 35.5 KB |
   | `ed25519/sc.o` | 15.7 KB |
   | `ed25519/fe.o` | 10.4 KB |
   | `ed25519/sha512.o` | 4.5 KB |
   | `aes128.o` + `sha256.o` | 3.9 KB |
   | `packet.o` + `identity.o` + `utils.o` | 1.8 KB |
   | rest of ed25519 | ~2 KB |
   | **Total** | **~73 KB** |

   LTO and `--gc-sections` will remove some of this. Whatever is left is handled by growing the `FLASH`
   region in `synthpass_ch572.ld` as needed, out of the 192 KB message-store reservation. → M2

4. **Parsing untrusted radio input isn't safe.** `unpack_packet` never checks `len`: a short frame
   underflows `len - i`, a `path_len` longer than the frame reads past the buffer, and a payload over
   184 bytes overflows `PM_Payload.data`. `MACThenDecrypt`/`decrypt` decrypt whole 16-byte blocks past
   `src_len` when the ciphertext isn't block-aligned, and the caller must make sure `dest` is large
   enough. On the host this is harmless. On a board receiving arbitrary 2.4 GHz frames, it corrupts
   memory. → M1
5. **Smaller correctness issues:**
   - `PUBLIC_CHANNEL_KEY` is a bare token, not a byte array.
   - MeshCore keys HMAC with a 32-byte secret. A 16-byte channel key must be zero-padded to 32 before it
     reaches `encryptThenMAC`, and nothing does this yet.
   - `hmac_sha256_finalize` takes `const uint8_t *out` but writes through it (compiler warning).
   - The `packet_calculate_hash` comment says the output is 32 bytes, but it writes 8.
   - `pack_pathlen`/`pack_header` have `// todo error?` branches that still return a packed value.

   → M1

### 5.2 Milestones

Every milestone has an exit criterion that a test can check. Host tests (`env:native`) cover logic and
interop vectors. Hardware tests (marked 🔧) cover what only the chip can show: size, timing, stack, RF.
The only 2.4 GHz MeshCore nodes that will exist are the ones we build, so interop is proven two ways:
**byte-exact vectors generated by MeshCore's own code** (`~/git/MeshCore`, built natively) on the host,
and **board-to-board exchange** on hardware.

#### M0: Green builds again (prerequisite, small)
- Stub out or remove the `config_*` calls in `SynthPass.c`, `radio.c` and `usb.c` so the firmware links.
  Minimal stubs returning defaults are enough. The real config rework comes later (M8).
- Add `extends = ch572_common` to the three variant envs.
- ~~Pinned framework vs. `../ch32fun` symlink~~ Decided 2026-09-27: keep the pinned package. It's
  current enough for SynthPass, so the symlink isn't needed.
- ~~Record a baseline `pio run` size for each env~~ Done 2026-09-27. Measured on the working tree on top of
  `efed1c9`, with MSC/`usb.c` removed, plain `fsusb` CDC, and config stubbed out:

  | Env | Flash used (of 48 KB region) | Static RAM (of 12 KB) | Free for stack |
  |---|---|---|---|
  | `genericCH572Q` | **9,664 B** (19.7%) | **3,956 B** (32.2%) | 8,332 B |
  | `nanoch57x` | 9,664 B (19.7%) | 3,956 B (32.2%) | 8,332 B |
  | `pretzelslab` | 9,744 B (19.8%) | 3,956 B (32.2%) | 8,332 B |
  | `axionqc` | — (`#error`: hardware not implemented yet) | — | — |

  Flash = `.init` 144 + `.highcode` 1,276 + `.text` 8,232 (8,312 on pretzelslab, which adds the QWIIC
  GPIO path) + `.data` 12. RAM = `.highcode` 1,276 (flash-write routines copied to RAM at boot) +
  `.data` 12 + `.bss` 2,668. The stack grows down from the end of RAM into whatever is left.
  PlatformIO's own summary (8,388 B flash / 2,680 B RAM) leaves out `.highcode` on both sides, so use
  the numbers above. Get them with `riscv-wch-elf-size -A .pio/build/<env>/firmware.elf`. Compared with
  the pre-M0 app (~16.6 KB flash / ~5.9 KB RAM), dropping MSC and the composite USB stack saved about
  7 KB of flash and about 2 KB of RAM.
- ~~Decide what happens to `spsource/`~~ Done 2026-09-27: removed. It was an untracked copy of the app
  at `bda8629`, identical byte for byte.
- **Exit:** `pio run` passes for all four board envs, and `pio test -e native` passes. Boop is expected
  to stay broken until config comes back (checked in M8).

#### M1: Harden and finish the pure-C protocol layer (host, plus USB CDC)
- Bring back USB as a plain CDC serial device, without the old CDC/MSC composite. M3's self-test
  output, M4's packet log and M5's commands all use it. 🔧
- Bounds-checked parsing: `unpack_packet` returns an error for short, over-long, or inconsistent frames.
  Decrypt rejects non-block-aligned ciphertext and takes an output buffer size. Invalid
  `pack_header`/`pack_pathlen` input returns an error.
- Key generation from a 32-byte seed (`ed25519_create_keypair`) plus a `PM_LocalIdentity` constructor.
  The seed source stays abstract so the host can use fixed seeds and the target can use the hardware RNG.
- Payload encode/decode for the types we'll use: **ADVERT** (pubkey, timestamp, signature, app-data
  flags/name with MeshCore's `AdvertDataHelpers` layout), **TXT_MSG**, **ACK**, **GRP_TXT**, **PATH**.
  Add the ACK-hash formula and the 32-byte channel-secret padding. Fix the `PUBLIC_CHANNEL_KEY`
  constant.
- Build a small native harness against `~/git/MeshCore/src` that emits known packets: a signed advert
  from a fixed key, a DM between two fixed keys, a public-channel GRP_TXT, and the matching ACK. Check
  them in as test vectors.
- Add fuzz-style tests: random and truncated frames through `unpack_packet` and `MACThenDecrypt` must
  never write out of bounds. Run the native env with `-fsanitize=address,undefined`.
- **Exit:** the host produces and consumes byte-identical packets to MeshCore for each supported payload
  type, and malformed-input tests pass under ASan/UBSan.

#### M2: Make room for the crypto (can run in parallel with M1)
1. **Measure properly.** Link the crypto into the real firmware (a stub call from `main` so LTO keeps
   it) and read the post-LTO `.map`. Standalone object sizes overstate the result.
2. **Expand the code region as needed.** Grow `FLASH` in `synthpass_ch572.ld` to fit the measured size
   plus headroom for M4–M7. Move `MSG_FLASH_ADDR` and shrink the message-store reservation to match.
3. **Check the smaller store still works.** Confirm the store's capacity (entry size × expected peers)
   is still enough.
4. **Optional size savings,** only if the region ever gets tight: replace `ge_scalarmult_base` to drop the
   30 KB base-point table, or switch to a size-optimized implementation (Monocypher, compact25519,
   TweetNaCl-derived). All M1 vectors must still pass.
- **Exit:** the firmware links with the crypto in use, and the new region size plus measured flash and
  RAM are recorded in this doc.

#### M3: Crypto and identity on target 🔧
- Add a separate `[env:selftest_ch572]`, like the `demo_ch572` pattern in the S3 interop work. At boot
  it runs the M1 vectors (hash, AES, HMAC, sign, verify, key exchange, keygen) and prints
  `SELFTEST PASS/FAIL <name> <µs>` lines over CDC, then idles. This is a clean way to run target tests
  without porting Unity's test runner over the USB stack.
- Measure each operation's time and the peak stack depth (fill the stack with a pattern and scan it
  afterwards). Key exchange and signing are the operations to watch: stack use against 12 KB of RAM,
  and whether signing blocks the radio loop long enough to miss RX.
- Generate the identity from the hardware RNG on first boot, run `local_id_validate_private_key`, and
  store the key in a dedicated flash sector next to the config sector. Loading must survive a reboot,
  and a re-flash through `flash_loop.sh`.
- **Exit:** all self-tests pass on at least two board variants, timing and stack numbers are recorded
  here, and the same public key shows up across a power cycle.

#### M4: MeshCore frames over iSLER 🔧
- Decide how mesh traffic shares the air with boops. Either use a separate access address/channel and
  time-slice RX, or share the SynthPass channel and tell the two apart in the payload. This is a design
  decision to write down here. A full MeshCore packet (≤255 bytes) fits in one iSLER frame, as the boop
  protocol already sends up to 255 bytes.
- RX path: iSLER frame → length and CRC check → `unpack_packet` → a small fixed-size RX queue, with RSSI
  saved. TX path: a queue drained from the main loop, with random jitter and an optional short RSSI
  listen-before-talk (replaces CAD, item 18).
- Periodic self-advert: build, sign, and flood an ADVERT. Received adverts get verified and saved in a
  small contacts table (pubkey, name, last-seen, out_path) held in RAM and persisted to flash.
- CDC debug output: one line per RX/TX packet (type, route, hash, RSSI). The same lines feed the hardware
  tests below.
- **Exit:** with two boards on the bench, each lists the other as a contact with a verified signature
  within N seconds of boot. A board whose advert is tampered with (a test build that flips a byte) is
  rejected. If boop is back by then, it must still work alongside mesh traffic.

#### M5: Direct messages and channels 🔧
- TXT_MSG encryption with the per-contact shared secret (cached per contact, not recomputed for every
  packet). Send the ACK on receipt, track the expected ACK hash on send, time out, and report to the
  host. MeshCore never retries on its own.
- GRP_TXT on the public channel and at least one hashtag channel.
- A minimal CDC command interface to send and read messages (`send <pubkey-prefix> <text>`,
  `chan <idx> <text>`, `contacts`, `msgs`). The same interface is what the CI tests drive.
- **Exit:** board A sends a DM to board B, B shows the plaintext, and A reports the delivery ACK. A
  public-channel message reaches B. On the host, the frames logged in these runs decode with the
  MeshCore-side harness from M1.

#### M6: Mesh routing 🔧
- Dedup cache of recently seen packet hashes (a fixed ring, sized from M2's RAM numbers).
- Flood relay: append our hop hash to the path, compute an RSSI-based `packetScore` and relay delay
  (the REWRITE items 14–16), cancel if we hear the same hash relayed by another node first, and respect
  the `do_not_retransmit` flag.
- Direct routing: forward only when we are the next hop. Learn `out_path` from received floods and PATH
  replies.
- Host simulation: an N-node simulator in `env:native` (a fake radio with a configurable link matrix)
  that tests relay, dedup, and path learning without hardware.
- **Exit:** the simulator passes the line, star and mesh topologies. On hardware, a 3-board line test
  (A↔B↔C, where A and C can't hear each other — use TX power at −25 dBm and distance or shielding)
  delivers A→C by flood, then by direct route with a returned ACK.

#### M7: Hardware CI (see `docs/hardware_ci_runner.md`)
Once M3 exists, most of that doc's work order can be done:
- Non-interactive `flash_one.sh <board-id> <env>` that reuses the `flash_loop.sh` helpers and returns an
  exit code plus JSON output (gap #5).
- Test driver (pytest or plain Python) that talks to the CDC interface from M4/M5 and asserts on
  output. It runs the M3 self-test, the M4 advert exchange, and the M5 DM/ACK round trip.
- Recovery from bricked boards (gap #4) **before** running untrusted branches unattended. Keep
  `iSLERStop()` before every bootloader jump.
- **Exit:** a `workflow_dispatch` job on the self-hosted runner flashes the fixture boards and runs the
  M3–M5 hardware tests from start to finish. `env:native` tests run on GitHub-hosted runners for every
  push.

#### M8: Follow-ups (not required for "protocol complete")
- **Config rework:** replaces the M0 `config_*` stubs. This one is planned; the rest of this list is
  optional.
- **Boop parity:** once config is back, boop behaves as it does on `main`. 🔧
- A subset of the companion-app serial protocol over USB CDC, so a MeshCore client that supports serial
  can drive a board. Check the flash cost after M2 first.
- SynthPass integration: exchange MeshCore public keys during a boop, so a boop adds a contact.
- A bridge to real LoRa MeshCore (for example, an ESP32 with both radios) for true interop with the
  wider network.
- Real BLE/GATT for the phone app is blocked by flash budget; see `docs/ble_stack_integration.md`.

### 5.3 Suggested order

```
M0 ──► M1 ─────────┐
   └─► M2 ─────────┴─► M3 ─► M4 ─► M5 ─► M6
                        └──────────► M7 (grows with each of M3–M6)
```

Do M0 first, since everything else needs a building firmware. M1 and M2 can run in parallel. The code
region grows as needed out of the message-store reservation, so flash size doesn't block later milestones.
