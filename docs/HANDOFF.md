# Build Handoff — start here

The "you are here, do this next" doc for a session picking up the build cold.
Design rationale lives in [`ARCHITECTURE.md`](ARCHITECTURE.md); this is state +
next steps only.

**Read order:** this doc → `ARCHITECTURE.md` → `README.md` →
[`FLASHING.md`](FLASHING.md) → [`PROJECT_BRIEF.md`](PROJECT_BRIEF.md).

**Repo:** https://github.com/underminedsk/baskets-lights · `pio test -e native`
(39 pass) and `pio run -e devkitc` / `-e firebeetle` build clean. **Uncommitted:**
the Stage-A radio duty-cycle + the SOLID boot-guard + the `GLOW` steady-color
pattern — all hardware-verified incl. a 12 V battery-side power measurement (below);
ready to commit.

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
  `pattern <n>`, `bri <n>`, `param <i> <v>`, `powersave on|off`.
- **Host unit tests** (`test/test_logic/`, 39): sync core, pattern math, roster,
  layout table, radio duty-cycle, glow warm-hue color.

**Hardware-verified (2026-06-28) — Milestone 3, Lever 1, Stage A (performer radio
duty-cycle):** a performer powers the radio **down** between brief listen windows
and keeps rendering from the synced clock, attacking the RX-dominated night draw.
Logic is the dependency-free `include/powersave.h` (5 host tests): a state machine
that holds the radio ON to acquire the first beacon, then cycles `DUTY_LISTEN_US`
ON (600 ms, spans ~2 of the 4 Hz beacons) / `DUTY_OFF_US` OFF (4 s) — ~13% radio
duty. `main.cpp` does the teardown/bring-up (`radioSleep`/`radioWake`:
`esp_wifi_stop`/`start`, re-adding the broadcast peer + recv-cb and re-flagging the
conductor unicast peer on each wake), gates TX on the radio being up, and feeds
caught-beacon events back to the scheduler. Conductor is exempt (gated on
`role == performer`). Toggle live with `powersave on|off` (persisted, NVS key
`ps`; default ON). The `[duty]` diag line reports `radio=ON/off` + `windows`/
`missed`.

**Bench result (conductor + duty-cycling performer, both DevKitC on USB):** radio
cycles ~0.6 s ON / ~4 s OFF as designed; **0% missed windows in steady state**
(every window catches a beacon and re-locks); the node free-runs the render across
each OFF with no blackout; wake reliably rebuilds the peer table (rx climbs across
sleeps); and the performer still appears in the conductor's `roster` (~10 s
cadence, mildly stretched because TX only happens during a window). The only
misses seen were transient, while the conductor itself was being reset during
setup. Note `gaps` increments ~once per wake (the first beacon after a 4 s sleep
has skipped seq numbers) — that's expected with duty-cycling and benign; the
`missed` counter is the meaningful health metric now, not `gaps`.

**SOLID boot-guard (same change):** a node never *boots* into `SOLID` (pattern 3,
full-white worst case) — `patternConfigLoad` falls a persisted SOLID back to
`SWEEP`, so a power-cycle can't leave a node draining the battery on all four
channels. `pattern 3` still works live for a deliberate on-bench measurement.

**Power result — MEASURED (2026-06-28, 12 V battery-side DMM, one DevKitC performer
locked to a beaconing conductor, steady-amber `GLOW` @ bri 48):**
- Radio **off** (rest, ~87% of the cycle): **51 mA @ 12 V**; radio **on** (the
  ~600 ms listen window, ~13%): **85 mA**. powersave-on **average ≈ 55 mA (~0.74 W
  @ 13.4 V)**; powersave-off pins the radio at the **85 mA** level (~1.14 W).
- Radio RX term = 85 − 51 ≈ **34 mA**; duty-cycling pays it only ~13% of the time
  → **saves ~30 mA @ 12 V (~0.4 W), ~35% of node draw.** The always-on 85 mA
  matches the original go/no-go's ~83 mA baseline (rigs agree).
- **Battery life (138 Wh, 10 h/night): ~12 → ~19 nights (~1.5×);** 24/7 calendar
  ~5 → ~7.8 days.
- **Why ~1.5× and not more:** the saving is a *fixed* ~30 mA radio term, but the
  ~51 mA rest floor (LEDs + CPU) now dominates, so the duty-cycle % scales inversely
  with LED load — a bigger win on dim shows. **Radio is no longer the dominant
  term;** the next levers are LED brightness, Stage B (CPU light-sleep to cut the
  floor), and Lever 2 (daytime deep-sleep).
- A *dim/5 V-side* sanity run under pulsing-white earlier showed the radio blip
  clearly (off-floor 0.05 A vs on 0.09–0.17 A); consistent with the above.

Measurement gotcha confirmed: do the battery reading with **USB disconnected**
(USB backfeeds 5 V into the DevKit and corrupts the 12 V draw) — powersave persists
in NVS, so set the mode over serial, then unplug and read. And never leave a USB
power meter inline on the data path (corrupts the UART / browns out radio init —
cost us a session; see FLASHING.md).

**A new show pattern landed alongside this:** `GLOW` (`pattern 4`) — a steady solid
color at a fixed hue, no time term, so the field holds one calm color with a *flat*
(non-pulsing) draw. `params[0]` = hue degrees (30 orange / 40 amber / 50 yellow),
`params[1]` = saturation %. Used as the realistic-conservative power-test scene; also
a genuine warm/gentle show pattern. Host test covers the warm-hue color math (39
tests now).

**Code layout:** `include/` — `config.h` (pins/constants), `beacon.h` (wire
packets), `sync.h` (clock core, tested), `pattern_math.h` (pure pattern fns,
tested), `patterns.h` (LED binding), `roster.h` + `table.h` (pure, tested),
`powersave.h` (radio duty-cycle schedule, pure, tested), `identity.h`
(NodeIdentity). `src/main.cpp`
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
daytime deep-sleep** — at 24/7 it's ~5 days, under a 10-night event. The photodiode
sets the duty cycle: at BRC (BM 2026 = Aug 30–Sep 7, ~40.8 °N) darkness is ~11 h
sunset→sunrise, so a dusk-tripped LDR runs **~10–10.5 h on / ~13.5 h asleep** —
the 10 h/night assumption holds; use 10.5 h for the post-M3 recompute (math in the
`power-budget-go-no-go` memory); (2) radio
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
- **Stage A — radio duty-cycle, CPU stays on. ✅ DONE + host-tested +
  hardware-verified** (see the bench result up top); ⏳ only the **power-draw
  number** is left — meter `powersave on` vs `off` on the 12 V side to record the
  saving. Built as `include/powersave.h` + glue in `main.cpp`; `powersave on|off`
  toggles it live.
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
