# rover — project context

ESP32 firmware: a `zenoh-pico` client for the classroom Robotics Hub
([`better-robotics/hub`](https://github.com/better-robotics/hub), Zenoh). Publishes
telemetry to the hub router; will serve the `led` RPC. Sibling layer to the hub —
this is the device end, C/ESP-IDF, not the Rust router.

## Build
- **PlatformIO + ESP-IDF** — zenoh-pico's official ESP-IDF path (its `library.json` +
  `extra_script.py` compile the lib). Pure `idf.py` won't work: zenoh-pico has no
  `idf_component_register`, so it's not a drop-in IDF component. `pio run -t upload`.
- Board: classic **ESP32-D0WD** (`az-delivery-devkit-v4`), CP2102, auto-reset.
- platform pinned **`espressif32@6.13.0`** (`<7.x` — 7.0 jumps to IDF 6 and breaks
  zenoh-pico's PIO build).

## zenoh-pico API notes (1.9.0)
- Config: `zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client")` +
  `Z_CONFIG_CONNECT_KEY, "tcp/<ip>:7447"`.
- `z_open(&s, z_move(config), NULL)` — read/lease tasks **auto-start** (multi-thread
  default); do NOT call `zp_start_read_task`/`zp_start_lease_task`.
- Publish: `z_bytes_copy_from_str(&payload, buf)` → `z_publisher_put(z_loan(pub), z_move(payload), NULL)`.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` — the 3584 default overflows `z_open` + the loop.

## Conventions
- **Measured data only** — publish only what the board truly measures (uptime, heap,
  chip temp). No faked IMU; this board has none. `synthetic:false`.
- **Config is provisional** — SSID + hub locator are hardcoded in `src/main.c`.
  Parameterizing them is roadmap (`better-robotics/hub#1`).

## Status
v1 publish-only (`robots/<id>/sys`). v2: `led` queryable + chip temp. Roadmap: `better-robotics/hub#1`.
