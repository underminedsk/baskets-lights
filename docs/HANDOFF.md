# Build Handoff — start here

The "you are here, do this next" doc for a session picking up the build cold.
Design rationale lives in [`ARCHITECTURE.md`](ARCHITECTURE.md); this is state +
next steps only.

**Read order:** this doc → `ARCHITECTURE.md` → `README.md` →
[`FLASHING.md`](FLASHING.md) → [`PROJECT_BRIEF.md`](PROJECT_BRIEF.md).

**Repo:** https://github.com/underminedsk/baskets-lights · everything is committed
and pushed; `pio test -e native` (33 pass) and `pio run -e devkitc` build clean.

---

## Where the build is right now

**Done & hardware-verified** (Milestones 1–2 → symmetric-role refactor → rainbow +
pattern persistence → protocol foundation Half 1 & 2 → battery go/no-go + worst-case
power measurement):
- **One firmware image for every node** (`src/main.cpp`); role is a runtime NVS
  value (default performer), set over serial. Build envs: `devkitc`, `firebeetle`,
  `native`.
- **Sync:** conductor broadcasts a clock beacon; performers lock an offset and
  render against synced time; **free-run on missed beacon** (no blackout), re-lock
  on return. Verified: `LOCKED`, stable offset ~±100 µs, `gaps=0`.
- **Patterns** (`f(x,y,t)`): `PULSE` (uniform breathing), `PALETTE_DRIFT` (smooth
  rainbow hue cycle; `params[0]`=period ms, `params[1]`=spatial hue offset ×100 so
  the rainbow travels or runs in unison), `SWEEP` (1-D traveling wave), and
  `SOLID` (`pattern 3`: every pixel full RGBW — the worst-case power draw, for
  bench-measuring the LED ceiling). Conductor broadcasts the recipe
  (`pattern_id`/`brightness`/`params[4]`); performers render it. Every node
  hard-clamps brightness to `MAX_BRIGHTNESS` (config.h, **192**) so no recipe can
  exceed the per-node power budget (see the worst-case measurement below).
- **NVS identity:** `id` + `(x,y)` persist across reboot; set over serial.
- **Pattern config persists** too: `pattern_id`/`brightness`/`params` survive a
  power-cycle (keys `pat`/`bri`/`p0`..`p3` in the `"node"` namespace).
- **Protocol foundation Half 1** (hardware-verified): typed message header
  `{magic, version, type}` with type dispatch; **MAC identity** read at boot and
  shown in `info`; **bidirectional ESP-NOW** — performers unicast `REGISTER`
  every 10 s and the conductor builds a **MAC-keyed roster** (`roster` command).
  Sync hot path unchanged (still `LOCKED gaps` flat after the rework).
- **Protocol foundation Half 2** (hardware-verified): **conductor-authoritative
  `MAC→(x,y)` layout table** in NVS, broadcast as chunked `MSG_TABLE`; a node finds
  its row, adopts its `(x,y)`, and caches it (survives reboot, no laptop needed).
  Conductor edits it with `assign <mac> <x> <y>` / `table` / `forget <mac>`.
  Verified: a node took a position set only on the conductor, no serial to it.
- **GPIO2 heartbeat** blinks on the synced beat (zero-wiring sync check).
- **Serial commands:** `info`, `roster` / `table` / `assign` / `forget`
  (conductor), `role conductor|performer`, `id <n>`, `pos <x> <y>`,
  `pattern <n>`, `bri <n>`, `param <i> <v>`.
- **Host unit tests** (`test/test_logic/`, 33): sync core, pattern math, roster,
  layout table.

**Code layout:** `include/` — `config.h` (pins/constants), `beacon.h` (wire
packets), `sync.h` (clock core, tested), `pattern_math.h` (pure pattern fns,
tested), `patterns.h` (LED binding), `roster.h` + `table.h` (pure, tested),
`identity.h` (NodeIdentity). `src/main.cpp`
is the on-device glue. NVS namespace is `"node"` (keys: `id`, `x`, `y`, `role`,
`pat`/`bri`/`p0`..`p3` for the recipe, `table` blob on the conductor).

**Not built yet:** structured machine Pi↔conductor serial (lands with the Pi UI),
auto-calibration, show program / scheduling, the Pi web UI, power management
(modem-sleep is on, but no deep-sleep/LDR/ADC), OTA.

## Hardware state

- 3× DOIT ESP32 DevKit V1, all on the unified image. Rings on 2 of them; LED data
  on **GPIO13 (`D13`)**, USB 5V (no 12 V / buck yet).
- Provisioned roles/ids/positions may have drifted across sessions — **re-check
  each board with `info`** rather than trusting labels.
- **Gotcha:** factory boards ship with ESP-AT firmware and need a one-time
  `esptool.py --port <P> erase_flash` before our image runs right. Boards report
  the same USB serial, so port names shuffle on replug — flash by current port.
  Full details in `FLASHING.md`.

**Power — battery go/no-go MEASURED (2026-06-28): GO (nighttime).** Wired the real
battery → buck → node chain (12 Ah LiFePO4 at 13.43 V → UCTRONICS 9–36V→5V buck →
DevKit performer + pulsing ring) and measured the **12V input** current with a DMM
in series. **Loaded draw ~83 mA → ~1.11 W total, converter included**; buck idle
(no board) 8.7 mA ≈ 0.12 W; **converter efficiency ~77%** (load = 0.855 W full-hour
ET900 integral ÷ 1.11 W input; UCTRONICS is fine — keep it). **~11 Wh/night →
138 Wh ÷ 1.11 W ≈ ~12 nights** at 10 h/night, clears the 10-night target. The
5V-side ET900 reading was the *load only* (0.855 W); this 12V number is
authoritative. Caveats: (1) **calendar life still needs
daytime deep-sleep** — at 24/7 it's ~5 days, under a 10-night event; (2) radio
likely dominates the draw — **modem-sleep is ineffective in connectionless ESP-NOW**
(`WIFI_PS_MIN_MODEM` set, CPU 160 MHz, but no AP/DTIM so RX stays on); the real
lever is **scheduled light-sleep between beacons** (synced clock enables it), the
Milestone-3 power item; (3) **FireBeetle** would draw less still. Full math in the
`power-budget-go-no-go` memory.

**Worst-case measured (2026-06-28, ET900 @ 5V):** `SOLID` (`pattern 3`) at
`bri 255` — every pixel full RGBW white — drew **0.76 A @ 5V = 3.8 W → ~4.6 W @ 12V
→ ~3 nights sustained** (~4× the colored show, since white lights all 4 channels).
**Battery GO holds** — even pathological all-white never fails in one night, and
0.76 A is well inside the buck/USB limits (so the cap is policy, not safety).
Decision: **`MAX_BRIGHTNESS = 192`** keeps worst case ~3.8 nights while barely
dimming real shows. Watts are fine; the gating issue is *hours* → daytime sleep.

## Quick reference

```bash
export PATH="/opt/homebrew/bin:$PATH"
pio test -e native                                  # 33 host tests
pio run -e devkitc                                  # build
pio run -e devkitc -t upload --upload-port /dev/cu.usbserial-XXXX
pio device monitor -p /dev/cu.usbserial-XXXX        # provision + watch
```
Reading serial without resetting the board: see the pyserial snippet in
`FLASHING.md` (opening the port auto-resets; wait ~2 s before typing commands).

---

**⚠ `main.cpp` global-ordering pitfall (for future edits):** NVS helpers must be
defined *below* the global they touch. `patternConfigLoad/Save` sit *after*
`g_beacon`; `tableLoad/Save` need `g_table`, which is declared up top with the
other config globals for exactly this reason. Don't move these loads into
`configLoad()` or forward-declare the globals — a prior attempt broke the build.

## Next task: Milestone 3 — power management

The protocol foundation (both halves) is **done, hardware-verified, and pushed**.
Battery is GO for nighttime (~12 nights); the open problems are (a) the active-night
draw is RX-dominated and (b) calendar life at 24/7 is only ~5 days. Two levers, in
this order:

### Lever 1 (do first): radio off between beacons — performer-only

**Why it works:** a performer free-runs `f(x,y,t)` from the synced clock, so it does
*not* need continuous RX — only periodic beacons for clock-drift correction and
recipe/table updates. So turn the radio **off** most of the time and wake it briefly
to resync. This attacks the dominant RX term (modem-sleep can't, see power note).
**Conductor is exempt** — it must beacon every 250 ms (TX), and is typically
wall-powered; gate all of this on `role == performer`.

**Staged plan (measure each stage on the ET900/12 V DMM):**
- **Stage A — radio duty-cycle, CPU stays on.** Every ~3–5 s: radio ON for a short
  listen window (~400–600 ms, guaranteed to catch ≥1 of the 4 Hz beacons) → resync +
  apply any recipe/table change → radio OFF; keep rendering from the synced clock the
  whole time (LEDs stay smooth, `esp_timer` keeps running so synced time stays
  coherent). Lower risk, biggest single win. **Start here.**
- **Stage B — add CPU light-sleep between rendered frames** for the remaining CPU
  draw. SK6812 latch their last color, so the LEDs hold during sleep; wake ~20–30 Hz
  to re-render. Harder: verify whether `esp_timer`/systimer advances across
  `esp_light_sleep_start()` — if not, add the slept RTC duration back or synced time
  drifts. Defer until Stage A is measured.

**Gotchas / open questions to resolve while building:**
1. **Radio on/off mechanism.** `esp_wifi_stop()`/`start()` (or `esp_now_deinit/init`)
   tears down peers — must re-add the broadcast peer (and conductor peer) on each
   wake; recv-cb registration may persist, peers don't. Measure the on/off latency
   and confirm a beacon is reliably caught in the window. This is the main risk —
   prototype the cycle first.
2. **Drift budget.** Free-running ~3–5 s on the last offset is sub-ms drift (crystals
   ~tens of ppm); fine for the slow patterns. Tune the off-interval for the
   power/accuracy/recipe-latency trade (a pattern change now lands up to one interval
   late — acceptable for an art piece, note it).
3. **Later refinement:** predict the next beacon time from the synced clock and wake
   *just* before it, shrinking the listen window further.

### Lever 2 (then): daytime deep-sleep — calendar life

Fixes the 24/7 ~5-day problem by sleeping through daylight. **Light sensor on
`PIN_LDR` = GPIO34 (ADC1 — ADC2 dies with the radio, already reserved in config.h).**
User leans toward a **photodiode/phototransistor** (faster than an LDR; an LDR in a
divider also works for a coarse threshold — pick by what's on hand). Below a light
threshold for a debounce → `esp_deep_sleep` (~10 µA), waking on an RTC timer to
re-sample (e.g. every ~30 min) or sleeping a fixed span until expected dusk. Add the
**battery ADC on `PIN_VBAT` = GPIO35** (divider) to report voltage + low-batt cutoff.

### After Milestone 3
Milestone 4 — battery enclosure + final go/no-go on the **FireBeetle** (lower draw
than the DevKit). Then the non-power tracks still open: auto-calibration (§6),
Pi web UI / control plane (§5.2, needs the deferred machine serial), show program
(§4.1). Milestone 5 — OTA + enclosure.

## Project memory (loaded automatically in this dir)

- `at-firmware-erase-flash` — erase new boards first; serial-port shuffle.
- `power-budget-go-no-go` — ET900 measurement plan, ~11 Wh/night target.
- `design-discussion-style` — in design mode, recommend in prose (no question widgets).
