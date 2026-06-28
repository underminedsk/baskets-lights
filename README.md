# Do Baskets Dream — Firmware

Node firmware for a Burning Man art installation: a field of 50–60 battery-powered
LED lanterns that play coordinated patterns (slow pulses, palette drifts) sweeping
across the physical field. Each lantern is an independent node; coordination is
wireless via **ESP-NOW** — there is no wiring between nodes.

Built with **PlatformIO** (Arduino framework) for the ESP32.

## Status

**Milestone 1 — sync proof (in progress):** 1 conductor + 2 performers, USB-powered,
showing a synchronized slow pulse over ESP-NOW.

See [`docs/do_baskets_firmware_brief.md`](docs/do_baskets_firmware_brief.md) for the
full project brief.

## Hardware (per node)

| Function | Part | Connection |
|---|---|---|
| MCU | ESP32-WROOM-32 (DevKitC or FireBeetle 2 ESP32-E) | — |
| LEDs | 16× SK6812 RGBW ring | GPIO18, 470Ω series resistor |
| Dusk sensor | LDR + 10kΩ divider | GPIO34 (ADC1) |
| Battery sense | 47kΩ/10kΩ divider off 12V | GPIO35 (ADC1) |
| Power | 12V LiFePO4 → buck → 5V | 5V to ESP32 VIN + ring; common ground |

> **Hard constraint:** sensing must use ADC1 pins (GPIO 32–39). ADC2 stops working
> when the radio is active.

## Roadmap

1. **Sync proof** — conductor + 2 performers, synchronized pulse, serial diagnostics.
2. (x,y) NVS identity + a pattern that sweeps across nodes by position.
3. Power management (modem-sleep, 160MHz, dusk deep-sleep) + LDR/battery ADC.
4. Battery power; validate runtime against the energy budget.
5. OTA via on-demand AP; enclosure.
