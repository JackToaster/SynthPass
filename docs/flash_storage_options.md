# Flash storage for the message store and config

Which lightweight embedded storage layer should hold received messages, config, and (with the MeshCore
work) identity, contacts and channels, in the ~100 KB of CH572 flash left after the code region grows.

Requirements, in priority order:

1. **A clean seam.** Business logic must not know which backend it's using, so native builds can use the
   host filesystem (or RAM) and other targets can use their own store (ESP32: LittleFS / NVS).
2. **Small code size** on the flash-constrained CH572.
3. Wear leveling is nice to have. At SynthPass message volumes it probably isn't needed (see §4).

USB mass storage is being dropped from the project, so it's out of scope here. Nothing below has to
produce a FAT view or zero-copy pointers into flash.

---

## 1. What the storage layer has to hold

| Data | Shape | Write pattern |
|---|---|---|
| Received messages | Append-only log of ≤256-byte records, walked in order | Append on receipt; clear all at once (`msgstore_received_clear`) |
| Own PROX / BOOP messages | 2 small named blobs | Rare rewrite |
| Config | One small struct (4 bytes today; reworked later) | Rare rewrite |
| MeshCore identity (M3) | 64-byte private key + pubkey | Written once |
| Contacts (M4) | N records: pubkey, name, `out_path`, last-seen | Updated when a path or name changes |
| Channels (M5) | A few 16/32-byte secrets + names | Rare |

That's a **key-value store plus one append-only log**. It isn't a hierarchical filesystem, and that
decides a lot of what follows.

## 2. CH572 flash facts that constrain the choice

From `src/msgstore.c` and `ch5xx_flash.h`:

- **Erase unit is 4 KB.** The CH570/572 branch of `ch5xx_flash_cmd_erase` has sector (4 KB), 32 KB and
  64 KB erase, but no 256-byte page erase like the CH58x.
- **Programming is 32-bit words**, in bursts of up to 256 bytes. It only programs erased flash, so
  there's no read-modify-write of a word.
- **Reads are memory-mapped**, so a backend's `read` callback is just `memcpy`.
- **Erase and program must run from RAM** (`__HIGH_CODE`). The existing `flash_erase_sector` and
  `flash_write` in `msgstore.c` already do this and would become the port layer for any backend. The
  library code itself can stay in flash.
- Budget: ~100 KB ≈ **25 sectors**.
- The datasheet copy in `docs/` doesn't state erase endurance.

## 3. Candidates, measured

Each library was built with the **exact firmware flags** (`-Os -march=rv32imc_zba_zbb_zbc_zbs_xw
-mabi=ilp32 -flto`, `--gc-sections`, nano specs). Each was linked into a small program that calls the
operations SynthPass would actually use, so unused code is stripped just as it would be in the firmware.
Port callbacks were stubs, so add a few hundred bytes of glue. Sources are current upstream as of
2026-09-27.

| Option | Flash (linked) | Static RAM | Native / host backend | ESP32 story | Wear leveling | License |
|---|---|---|---|---|---|---|
| **Current bespoke `msgstore.c`** | ~4.6 KB (unlinked `.o`, incl. README strings) | ~8 B + RAM-resident flash routines | None; would need writing | None | None | project |
| **FlashDB** KVDB only (v2.x) | **7.0 KB** | 1.0 KB (default caches) | Built-in file mode (`FDB_USING_FILE_POSIX_MODE` / `_LIBC_MODE`), same API | Upstream demo `demos/esp32_spi_flash` | Sector rotation + GC | Apache-2.0 |
| **FlashDB** KVDB + TSDB | **8.8 KB** | 1.15 KB, or **0.58 KB** with caches cut to 8/4 entries | same | same | same | Apache-2.0 |
| **LittleFS** 2.11 | **14.7 KB** | ~0.5 KB (64 B caches) + 64 B per open file | `bd/lfs_filebd`, `lfs_rambd`, `lfs_emubd`, or plain POSIX | **The current ESP-IDF default** (`joltwallet/littlefs` component; the IDF's own `examples/storage/littlefs` uses it) | Dynamic, via `block_cycles` | BSD-3 |
| LittleFS read-only | 6.5 KB (unlinked) | — | — | — | — | — |
| **SPIFFS** | ~24.9 KB (unlinked, no cache) | — | — | Legacy in ESP-IDF | Yes | MIT |

Not measured: Zephyr NVS (small, but tied to Zephyr's `flash_area` API) and ESP-IDF NVS (C++, far too
large for the CH572). Either is fine *as the ESP32 backend* behind the seam in §5.

**SPIFFS is out.** It's the largest, it's effectively unmaintained upstream, and ESP-IDF treats it as
legacy.

### 3.1 FlashDB

KVDB is a key-value store keyed by string, with blob values. TSDB is an append-only log of
timestamped records with iterate, iterate-by-time and clean, plus an optional per-record status
(e.g. DELETED). **Those two map directly onto the data in §1.**

Upsides:
- The smallest robust option. It's power-fail safe and handles GC and sector rotation, which the
  bespoke store doesn't.
- The KV half covers identity, contacts, channels and config. The bespoke store has no way to *update*
  a record, and contacts need that.
- File mode gives native tests the same code path as the target, not just the same interface.

Costs and gotchas:
- It goes through FAL (a flash abstraction layer with a partition table). That's ~0.8 KB of the numbers
  above. The CH572 FAL device is ~20 lines wrapping the existing RAM-resident erase/program functions.
- Set **`FDB_WRITE_GRAN 32`**. Status fields then each sit in their own word and are programmed exactly
  once from the erased state, which matches the CH572's word-program-only flash. **Check this on
  hardware in the M3 self-test.** It's the one assumption that isn't proven for this flash controller.
- TSDB needs a monotonically increasing timestamp. There's no RTC, so use a counter persisted in KVDB
  (or boot count + uptime).
- TSDB's maximum record length is fixed at init. Use 256 + header, which matches `MAX_DATA` today.
- Individual message deletion is status-marking only. Space comes back only on clean or when the log
  rolls over (rollover is optional). That fits the current "delete all" behavior.
- Turn logging off (`FDB_PRINT` / `FAL_PRINTF` empty), or it pulls in printf strings: 12.7 KB instead
  of 8.8 KB.
- KVDB needs ≥2 sectors and TSDB ≥2 sectors, since each keeps a spare for GC.
- The community is smaller than LittleFS's, but the project is active (last upstream commit
  2026-09-23).

### 3.2 LittleFS

A real hierarchical filesystem: power-loss safe, bounded RAM, no recursion, and the de facto standard
(ESP-IDF, Zephyr, Pico, nRF). Its portability is the best of the options.

For this workload on this flash, though:
- **It's about 2× FlashDB's size** (14.7 KB vs 7–8.8 KB).
- **4 KB blocks are a poor fit for small appends.** Every non-inline file owns at least one whole
  block. Appending after a sync can't program the free tail of the block in place, so LittleFS copies
  it. Storing messages one per file wastes a block each. Storing them in one log file copies up to
  4 KB per appended message. Small files can be stored *inline* in directory metadata, but many of
  them in one directory make metadata compaction the hot path (`metadata_max` / `compact_thresh`
  mitigate this). It works, but it's working against the grain.
- The advantage (a file API) matters mostly for things SynthPass is dropping, such as mass storage.

### 3.3 Keep the bespoke store

It's the smallest, and it already works for the log. But it has no update-in-place, no power-fail
handling (a torn append leaves a record with valid magic and a bad length), and no wear rotation.
MeshCore's contacts and identity would need a KV layer built next to it, which is where FlashDB's ~7 KB
goes anyway. It's a reasonable **first backend behind the seam** if you want to decouple the interface
work from the library switch (§6, step 1).

## 4. Does wear leveling matter?

Rough numbers: 100 received messages a day × ~270 bytes ≈ 27 KB/day. The whole ~100 KB region fills
in about 4 days, so a rolling log erases each sector **~90 times a year**. That's orders of magnitude
under any NOR endurance figure. Any wear risk comes from **KV churn**, not messages. For example,
writing a contact's last-seen on every advert would rewrite the same keys constantly. Keep volatile
fields such as last-seen and RSSI in RAM, and persist only on real changes (new contact, new path, new
name). With that rule, FlashDB's sector rotation is more than enough.

## 5. The seam: a store interface shaped like the data

Don't make the interface a POSIX-style file API. That forces a filesystem onto every backend, when what
the app actually uses is KV + log. Use a small project-owned header. Each backend lives in its own
`src/store_<backend>.c`, and PlatformIO's `build_src_filter` picks one per env, the way `env:native`
already selects files.

```c
// store.h — the only storage API business logic may include
int  store_init(void);

// key-value: config, identity, contacts, channels, own messages
int  kv_get(const char *key, void *buf, size_t cap, size_t *len);  // 0 ok, <0 missing/error
int  kv_set(const char *key, const void *val, size_t len);
int  kv_del(const char *key);

// append-only log: received messages
int  log_append(const void *rec, size_t len);
typedef int (*log_visit_fn)(uint32_t seq, const void *rec, size_t len, void *ctx); // return !0 to stop
int  log_foreach(log_visit_fn fn, void *ctx);
int  log_clear(void);
```

| Env | Backend | Notes |
|---|---|---|
| CH572 | `store_flashdb.c` (FAL device over the existing `__HIGH_CODE` erase/program) | Or `store_msgstore.c` as a stopgap |
| `native` | `store_ram.c` (arrays, for unit tests) and/or FlashDB file mode | RAM is simplest for tests; file mode exercises the real code path |
| ESP32 (if added) | FlashDB on `esp_partition`, **or** ESP-IDF NVS for `kv_*` plus a LittleFS file for `log_*` | Either works behind the same header |

This also covers the "can substitute a native filesystem" requirement without committing the CH572 to
a filesystem it doesn't need.

## 6. Recommendation

**Use FlashDB (KVDB + TSDB) on the CH572, behind the `store.h` seam.** That's ~8.8 KB of flash and
~0.6–1.2 KB of RAM depending on cache sizes. It's the smallest robust option, its two primitives are
exactly the app's data model, it's power-fail safe with sector rotation, and its file mode keeps
native tests honest. Choose LittleFS instead only if a real file API becomes a requirement. It costs
~6 KB more and fits 4 KB-block small appends poorly.

Suggested order:
1. Write `store.h` and `store_ram.c`, and move `msgstore.c` callers onto it, with the current flash
   layout behind a `store_msgstore.c` adapter. No behavior change, and the seam exists.
2. Add `store_flashdb.c` with the CH572 FAL device (`write_gran` 32, logging off). Run it on the native
   env first via file mode, with the same tests as the RAM backend.
3. On hardware (fits naturally into M3's self-test env): format, append/iterate/clear, KV set/get
   across reboot, and a power-cut-during-append test (pull USB mid-write in a loop).
4. Size the partitions against the new region split from M2. For example, KV 3–4 sectors and TSDB the
   rest.

**Confidence.** The sizes are measured with this project's exact toolchain and flags, but in stub
programs, not the real firmware, so expect ±10% after integration. FlashDB's `write_gran 32` behavior
on CH572 flash and the power-cut behavior are unverified until step 3. The LittleFS append-copy
behavior is from its documented design (copy-on-write blocks), not measured here.
