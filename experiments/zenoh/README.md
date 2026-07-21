# Zenoh firmware spikes — hardware-proven reference

Bring-up spikes for the **MQTT → Zenoh transport migration**
(tracked in [`better-robotics/hub#9`](https://github.com/better-robotics/hub/issues/9);
wire spec: `hub/zenoh-migration.md`). These are **not** production firmware — the
config is hardcoded and they don't touch NVS/naming/motor pins. They exist to
prove the transport works on real silicon and to hand the production port
(swapping `esp-mqtt` → `zenoh-pico` inside `rover_role.c` / `hub_role.c`) a
working recipe.

- **`rover/rover.c`** — a `zenoh-pico` **client** rover: joins the hub AP,
  connects to a Zenoh router/hub, publishes `robots/<id>/sys`+`imu`, subscribes
  `robots/<id>/pwm` (drives the onboard LED) and `fleet/estop`, and answers a
  `robots/<id>/led` queryable (the `set_led` RPC).
- **`hub/hub.c`** — the **ESP-hub role**: hosts an open SoftAP, runs `zenoh-pico`
  in **peer mode with a TCP listen endpoint** (`Z_FEATURE_UNICAST_PEER`), so
  rovers connect straight to it. Subscribes `robots/**` (uplink) and publishes
  `robots/<id>/pwm` (downlink) — no Pi, no `zenohd`.

## Validated on hardware (2026-07-21)

- **Pi tier**: rover ⟷ `zenohd` (on a Pi 4) ⟷ MCP bridge — `fleet`/`imu`/drive,
  the `set_led` queryable, and the `fleet/estop` latch all round-trip.
- **ESP-hub tier**: rover ⟷ `hub.c` (on an ESP32-C3) — telemetry up **and**
  commands down, both directions, no Pi.

Rover firmware: **879 KB** (43% of the A/B OTA slot). Hub: **920 KB** (45%).

## The three gotchas the production port MUST carry

These are runtime traps the upstream `examples/espidf/*` (compile-only in CI) hide:

1. **Main-task stack**: `z_open` overflows the ESP-IDF default 3584 B main stack
   → `LoadProhibited` crash at session open. Set
   `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` (see `sdkconfig.defaults`).
2. **Read/lease tasks are not optional and `auto_start` didn't run them**: call
   `zp_start_read_task` + `zp_start_lease_task` explicitly after `z_open`.
   Without the read task the node can publish but **receives nothing** (subs and
   queryables go silent); without the lease task the router drops it at lease
   expiry.
3. **Power save vs the Pi's brcmfmac AP**: default STA power-save makes the AP
   drop the association right after assoc → `esp_wifi_set_ps(WIFI_PS_NONE)`.

## Build

Point config at your hub in the two `.c` files (`WIFI_SSID` / `ZENOH_CONNECT`
for the rover; the hub auto-names its AP `hub-<mac>`), then, in a PlatformIO
ESP-IDF project:

```ini
[env:esp32dev]           ; rover: classic ESP32 devkit  (hub: esp32-c3-devkitm-1)
platform = espressif32@6.13.0
framework = espidf
board_build.partitions = ../../partitions.csv
build_flags = -DZENOH_ESPIDF -DZ_FEATURE_UNICAST_PEER=1
lib_deps = https://github.com/eclipse-zenoh/zenoh-pico
```

Copy `sdkconfig.defaults` alongside `platformio.ini`, put the target `.c` in
`src/`, then `pio run -t upload`.
