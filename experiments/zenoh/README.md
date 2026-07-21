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
- **`hub-ws/hub.c`** — the ESP hub **plus the browser edge**: the same peer-listen
  hub with the **WS-JSON adapter** the dashboard needs (a WebSocket on `:9001`
  mapping `{op:pub|sub|get|auth}` JSON onto the hub's local zenoh session). This
  is the sibling of `../../src/ws_mqtt_bridge.c` — same `httpd`/WS termination and
  bounded slot pool — but the byte-pump-to-broker is replaced by op parsing,
  because Zenoh has no on-chip broker socket to pipe to. Adds an instructor
  auth gate on `fleet/estop` writes and a `**`-subscriber that fans samples out
  to matching clients. Needs `CONFIG_HTTPD_WS_SUPPORT=y` (compile-gated off by
  default). Closes the last architectural gate in `hub/zenoh-migration.md`.
- **`ws-client-rig/main.c`** — an `esp_websocket_client` test rig that stands in
  for the browser **on real silicon**, so testing never has to join the laptop's
  Wi-Fi to the hub AP (which drops the user's internet). Joins `hub-f825`, drives
  the adapter through sub/pub/get/auth, and asserts each leg (heartbeat + rover
  telemetry fan-out, the auth gate, downlink drive, the `set_led` queryable) on
  its serial log.

## Validated on hardware (2026-07-21)

- **Pi tier**: rover ⟷ `zenohd` (on a Pi 4) ⟷ MCP bridge — `fleet`/`imu`/drive,
  the `set_led` queryable, and the `fleet/estop` latch all round-trip.
- **ESP-hub tier**: rover ⟷ `hub.c` (on an ESP32-C3) — telemetry up **and**
  commands down, both directions, no Pi.
- **Browser edge** (`hub-ws/`): **boot + coexistence verified** on a C3 — the
  adapter firmware comes up as `hub-f825`, opens its zenoh peer + read/lease
  tasks, stands up the WS adapter on `:9001` and the page on `:80`, stays
  healthy (heap ~164 KB), and the rover re-joins its AP. The ESP-specific risk
  (an `httpd` WS server sharing the chip with zenoh-pico's own tasks/sockets) is
  retired. The end-to-end round-trip *through* the adapter (via `ws-client-rig/`)
  is built and staged; run it once the bench USB is stable to close the proof.

Rover firmware: **879 KB** (43% of the A/B OTA slot). Hub: **920 KB** (45%).
Hub-with-adapter: **997 KB** (49%). Test rig: **954 KB**.

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
