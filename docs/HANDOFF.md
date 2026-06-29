# Build Handoff — start here

The "you are here, do this next" doc for a session picking up the build cold.
Design rationale lives in [`ARCHITECTURE.md`](ARCHITECTURE.md); this is state +
next steps only.

**Read order:** this doc → `ARCHITECTURE.md` → `README.md` →
[`FLASHING.md`](FLASHING.md) → [`do_baskets_firmware_brief.md`](do_baskets_firmware_brief.md).

**Repo:** https://github.com/underminedsk/baskets-lights · everything is committed
and pushed; `pio test -e native` (25 pass) and `pio run -e devkitc` build clean.

---

## Where the build is right now

**Done & hardware-verified (Milestones 1–2):**
- **One firmware image for every node** (`src/main.cpp`); role is a runtime NVS
  value (default performer), set over serial. Build envs: `devkitc`, `firebeetle`,
  `native`.
- **Sync:** conductor broadcasts a clock beacon; performers lock an offset and
  render against synced time; **free-run on missed beacon** (no blackout), re-lock
  on return. Verified: `LOCKED`, stable offset ~±100 µs, `gaps=0`.
- **Patterns** (`f(x,y,t)`): `PULSE` (uniform breathing), `PALETTE_DRIFT` (smooth
  rainbow hue cycle; `params[0]`=period ms, `params[1]`=spatial hue offset ×100 so
  the rainbow travels or runs in unison), and `SWEEP` (1-D traveling wave).
  Conductor broadcasts the recipe (`pattern_id`/`brightness`/`params[4]`);
  performers render it.
- **NVS identity:** `id` + `(x,y)` persist across reboot; set over serial.
- **Pattern config persists** too: `pattern_id`/`brightness`/`params` survive a
  power-cycle (keys `pat`/`bri`/`p0`..`p3` in the `"node"` namespace).
- **Protocol foundation Half 1** (hardware-verified): typed message header
  `{magic, version, type}` with type dispatch; **MAC identity** read at boot and
  shown in `info`; **bidirectional ESP-NOW** — performers unicast `REGISTER`
  every 10 s and the conductor builds a **MAC-keyed roster** (`roster` command).
  Sync hot path unchanged (still `LOCKED gaps` flat after the rework).
- **GPIO2 heartbeat** blinks on the synced beat (zero-wiring sync check).
- **Serial commands:** `info`, `roster` (conductor), `role conductor|performer`,
  `id <n>`, `pos <x> <y>`, `pattern <n>`, `bri <n>`, `param <i> <v>`.
- **Host unit tests** for the sync core + pattern math (`test/test_logic/`).

**Code layout:** `include/` — `config.h` (pins/constants), `beacon.h` (wire
packet), `sync.h` (clock core, tested), `pattern_math.h` (pure pattern fns,
tested), `patterns.h` (LED binding), `identity.h` (NodeIdentity). `src/main.cpp`
is the on-device glue. NVS namespace is `"node"` (keys: `id`, `x`, `y`, `role`).

**Not built yet:** the layout table (`MAC→(x,y)` broadcast + NVS cache) and
structured Pi↔conductor serial (Half 2, next), auto-calibration, show program /
scheduling, the Pi web UI, power management (modem-sleep is on, but no
deep-sleep/LDR/ADC), OTA.

## Hardware state

- 3× DOIT ESP32 DevKit V1, all on the unified image. Rings on 2 of them; LED data
  on **GPIO13 (`D13`)**, USB 5V (no 12 V / buck yet).
- Provisioned roles/ids/positions may have drifted across sessions — **re-check
  each board with `info`** rather than trusting labels.
- **Gotcha:** factory boards ship with ESP-AT firmware and need a one-time
  `esptool.py --port <P> erase_flash` before our image runs right. Boards report
  the same USB serial, so port names shuffle on replug — flash by current port.
  Full details in `FLASHING.md`.

**Power — first bench measurement (2026-06-28, ET900 on USB 5V):** a *performer*
on a DevKit V1, **LED ring connected, gentle pulsing color-wheel pattern**, draws
**0.17 A = 0.85 W** — the **full per-node draw** (board + radio + LEDs).
**Budget passes:** 0.85 W × 10 h = 8.5 Wh/night (under the 11 Wh target); 138 Wh ÷
0.85 W ≈ **~16 nights** at 10 h/night, even on the inefficient DevKit. Caveats:
(1) confirm 0.17 A as a true average via the ET900 mAh accumulator (LEDs pulse →
current swings); (2) radio likely dominates — **modem-sleep is ineffective in
connectionless ESP-NOW** (`WIFI_PS_MIN_MODEM` set, CPU 160 MHz, but no AP/DTIM so
RX stays on); the real lever is **scheduled light-sleep between beacons** (synced
clock enables it), a Milestone-3 margin item; (3) **FireBeetle** would draw less;
(4) **calendar life still needs daytime deep-sleep** — at 24/7 it's ~6.8 days,
under a 10-night event. Full math in the `power-budget-go-no-go` memory.

## Quick reference

```bash
export PATH="/opt/homebrew/bin:$PATH"
pio test -e native                                  # 25 host tests
pio run -e devkitc                                  # build
pio run -e devkitc -t upload --upload-port /dev/cu.usbserial-XXXX
pio device monitor -p /dev/cu.usbserial-XXXX        # provision + watch
```
Reading serial without resetting the board: see the pyserial snippet in
`FLASHING.md` (opening the port auto-resets; wait ~2 s before typing commands).

---

**⚠ NVS pattern-config pitfall (for future edits to `main.cpp`):** `configLoad()`
is defined *above* `g_beacon`, so it can't touch it. The pattern recipe is
loaded/saved by separate `patternConfigLoad()` / `patternConfigSave()` defined
*after* `g_beacon` (called from `setup()` and the `pattern`/`bri`/`param`
handlers). Don't move pattern loads into `configLoad()` or forward-declare
`g_beacon` — a prior attempt broke the build that way.

## Next task (big): protocol foundation — Half 2

Half 1 (typed header, MAC identity, bidirectional registration + roster) is **done
and hardware-verified** — see the status list above. The wire substrate now exists
(`beacon.h`: `MsgHeader` + `MsgType`; `MSG_ROSTER`/`MSG_TABLE`/`MSG_ACK` reserved
but unused). Half 2 builds the layout table on top. See `ARCHITECTURE.md` §5, §5.2.

1. **Routing table in conductor NVS.** Store as a blob of `{uint8 mac[6]; float x;
   float y;}` rows. Broadcast as `MSG_TABLE` chunks (≤~250 B → ~17 rows/packet),
   occasionally. Each node scans for its MAC and caches `(x,y)` to NVS, so it
   adopts its position with no serial. Conductor is authoritative; field runs with
   no laptop. The roster (`g_roster`, MAC-keyed) is the natural seed for the table.
2. **Structured serial protocol (Pi↔conductor).** Beyond the human commands, add a
   machine protocol with bulk **table get/set** (60 rows won't fit a typed line),
   show-program get/set, and calibration control, with clean acks/errors.

**Verify like Half 1:** set a node's `(x,y)` by MAC from the conductor and watch
the node adopt + cache it without ever talking to that node directly.

## After that

Auto-calibration (drone + CV; `ARCHITECTURE.md` §6) → Pi web UI (§5.2) →
Milestone 3 power management (modem-sleep tuning, dusk deep-sleep, LDR + battery
ADC on GPIO34/35) → Milestone 4 battery + **ET900 draw measurement = battery
vs. wired go/no-go** → Milestone 5 OTA + enclosure.

## Project memory (loaded automatically in this dir)

- `at-firmware-erase-flash` — erase new boards first; serial-port shuffle.
- `power-budget-go-no-go` — ET900 measurement plan, ~11 Wh/night target.
- `design-discussion-style` — in design mode, recommend in prose (no question widgets).
