# Do Baskets Dream — Firmware Project Brief

A Burning Man art installation: a field of 50–60 LED lanterns that play coordinated
patterns (slow pulses, palette drifts) that sweep across the physical field. Each
lantern is an **independent, battery-powered node**. There is no wiring between
nodes — coordination is wireless via **ESP-NOW**. This brief is the spec for the
node firmware; build it in PlatformIO.

---

## Goal of this phase

Get a **conductor + 2 performers** showing synchronized pulses over ESP-NOW, powered
by USB, before any battery/enclosure work. Sync is the hard part and the real risk —
prove it first, in isolation.

---

## Hardware (per node)

| Function | Part | Connection |
|---|---|---|
| MCU | ESP32-WROOM-32 — comparing **DevKitC** vs **FireBeetle 2 ESP32-E** (same chip, code runs unchanged on both) | — |
| LEDs | 16× **SK6812 RGBW** ring | data on **GPIO18**, through a 470Ω series resistor |
| LED power cap | 1000µF across the ring's 5V/GND | at the ring |
| Dusk sensor | LDR + 10kΩ divider | **GPIO34** (ADC1) |
| Battery sense | 47kΩ/10kΩ divider off the 12V line | **GPIO35** (ADC1) |
| Power | 12V LiFePO4 (TalentCell 12Ah) → buck → 5V | 5V to ESP32 VIN + ring; common ground |

**Hard pin/ADC constraint:** the LDR and battery sense **must** use ADC1 pins
(GPIO 32–39). ADC2 pins stop working whenever the WiFi/ESP-NOW radio is active —
this is a silent failure, not an error. GPIO34 and GPIO35 are input-only ADC1 pins.

---

## Architecture

**Roles.** One **conductor** broadcasts sync beacons; all others are **performers**.
Role is selectable via a build flag or an NVS value (default performer). The conductor
can be a headless 3rd ESP32 on USB, or a performer that also originates the beacon.

**Transport.** ESP-NOW broadcast (peer = `FF:FF:FF:FF:FF:FF`), `WIFI_STA` mode, a
**fixed WiFi channel** set explicitly on every node (`esp_wifi_set_channel`) so they
all agree without scanning.

**Clock model (this is the resilience core).** The conductor periodically broadcasts
its clock. Performers compute an offset and render against the synced time. Crucially,
a performer that **misses a beacon keeps free-running on its last known offset** and
re-locks on the next beacon — so a dropped packet causes at most slight drift, never a
blackout. Use **`esp_timer_get_time()` (64-bit microseconds)**, not `millis()`, to
avoid 32-bit wraparound over a multi-day run.

**Beacon packet (sketch — refine as needed):**
```c
typedef struct {
  uint32_t magic;        // identify our packets
  int64_t  epoch_us;     // conductor's clock at send time
  uint16_t pattern_id;
  uint8_t  brightness;   // global cap
  uint8_t  palette_id;
  uint16_t params[4];    // pattern-specific knobs for live tweaking
  uint32_t seq;          // for drop detection / logging
} Beacon;
```

**Position-aware rendering.** Each node stores its **node ID + (x,y) coordinate** in
NVS (non-volatile flash). Patterns render as **`f(x, y, t)`** so a pulse can physically
travel across the field. A node cold-boots, reads its (x,y) from NVS, hears a beacon
within ~1–2s, locks the clock, and resumes — so battery swaps look like a single blink.

**LED library.** Use **NeoPixelBus** (handles SK6812 RGBW cleanly). FastLED is fine if
preferred but its RGBW support is rougher.

**Power discipline (load-bearing for the battery budget — not optional).**
- Enable **WiFi modem-sleep** so the radio naps between beacons. Leaving it in
  continuous receive roughly doubles ESP32 draw (~0.25W → ~0.55W) and would push a
  node past its 10-night battery budget.
- Run the CPU at **~160MHz** (16 pixels need nothing more); saves ~15–30mA.
- **Deep-sleep through the day**, wake at dusk (LDR or RTC timer).
- Target: ESP32 ≈ 0.25W active at night.

**OTA (later phase).** On-demand WiFi AP (button or scheduled window) → ArduinoOTA.
Don't depend on field-wide WiFi.

---

## Milestones (in order)

1. **Sync proof (do this first):** 1 conductor + 2 performers, USB-powered, no battery,
   no LEDs-in-enclosure. All three show a synchronized slow pulse on their rings.
   **Print sync status to serial** (offset, last-beacon age, seq gaps) so the boards can
   be range-walked to find where sync drops.
2. Add the (x,y) NVS identity + a pattern that visibly sweeps across nodes by position.
3. Add power management (modem-sleep, 160MHz, dusk deep-sleep) and the LDR/battery ADC.
4. Move to battery power; validate runtime/energy against the budget below.
5. OTA via on-demand AP; enclosure.

**Starter patterns:** (a) slow brightness pulse, (b) palette drift. Both keyed to
synced time + (x,y).

---

## Tooling

- **PlatformIO**, Arduino framework.
- Two board environments: `esp32dev` (DevKitC) and the FireBeetle 2 ESP32-E.
- USB-serial driver may be needed (CP2102 or CH340) — and the USB cable must be a
  **data** cable, not charge-only.
- Deps: NeoPixelBus; ESP-NOW via `esp_now.h` (built in).
- Role flag (conductor/performer) exposed as a build flag for easy reflashing.

---

## Design targets / context (so firmware respects the physical design)

- Brightness tier: **gentle** (slow fades, low brightness) — fits the aesthetic and the
  battery budget. ~0.7W LEDs.
- Per-node nightly energy ≈ **11 Wh** (10h run) → 12Ah LiFePO4 gives ~12–13 nights,
  clearing a 10-night target with buffer. Keeping the ESP32 efficient is what protects
  this margin.
- Field: ~50–60 nodes; ESP-NOW single-hop from a centrally-placed (ideally elevated)
  conductor. Enable ESP-NOW long-range mode + max TX power if range testing demands it.

## Don't-break list

- ADC1 pins only for sensing (radio kills ADC2).
- Common ground between ESP32 and ring or the data line is invalid.
- Modem-sleep stays on — it's a battery-budget requirement, not a nicety.
- Free-run-on-missed-beacon behavior must hold (no blanking when a packet drops).
- 64-bit microsecond time base (no 32-bit millis wrap over the event).
