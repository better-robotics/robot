# rover — project context

ESP32 firmware: a `zenoh-pico` client for the classroom Robotics Hub
([`better-robotics/hub`](https://github.com/better-robotics/hub), Zenoh). Publishes
telemetry to the hub router; will serve the `led` RPC. Sibling layer to the hub —
this is the device end, C/ESP-IDF, not the Rust router.

## Build
- **PlatformIO + ESP-IDF** — zenoh-pico's official ESP-IDF path (its `library.json` +
  `extra_script.py` compile the lib). Pure `idf.py` won't work. `pio run -t upload`.
- Three envs: `esp32dev` (classic ESP32-D0WD devkit, CP2102, BOOT=GPIO0),
  `esp32c3-supermini` (ESP32-C3 QFN32, native USB-Serial/JTAG, BOOT=GPIO9 via
  `-DBUTTON_GPIO`), `esp32cam` (AI-Thinker ESP32-CAM — no USB socket, flashed
  via a plug-in USB↔UART adapter; **no BOOT button**, so provisioning re-entry
  is only via the join-failure fallback; LED=GPIO33 rear red, active-low).
- Platform pinned **`espressif32@6.13.0`** (`<7.x` — 7.0 jumps to IDF 6 and breaks
  zenoh-pico's PIO build).
- **Custom `partitions.csv`** — BLE + Wi-Fi + zenoh-pico together exceed the 1 MB
  `single_app_large` default; custom table gives the app 3 MB.

## Boot flow

Mode dispatch is **stateless**: NVS holds only credentials; behavior is a pure
function of "is config complete" plus a one-shot provision request in RTC noinit
RAM (survives `esp_restart()`, not a power cycle — no stale mode state can strand
a robot). One radio path per boot.

```
boot ── no config ──► provisioning mode (BLE, no timeout)
  │ config complete
  ▼
operating mode: Wi-Fi STA → z_open → publish robots/rover-XXXX/sys every 2s
  │ join fails (30s) · z_open fails · 5 consecutive put failures · button
  ▼
provisioning window (BLE, 3 min; extends while a client is connected)
  │ window expires (or button)
  ▼
esp_restart() → retry stored creds        ← transient outages self-heal
```

**BOOT button (GPIO0, hold ~1 s):** operating → provisioning window;
provisioning → retry Wi-Fi now. The human within arm's reach is the out-of-band
channel — this is its API (covers wedged states the firmware can't self-detect).

**Locator is provisioned, not discovered** — Zenoh scouting (UDP multicast
`224.0.0.224:7446`) is blocked on the target networks (campus Wi-Fi filters
multicast and isolates clients), so the rover must be told its one bootstrap
locator over BLE alongside Wi-Fi creds.

## Identity
`rover-XXXX` derived from last 2 bytes of Wi-Fi MAC via `rover_format_robot_id`.
Zero config — each board self-identifies at boot.

## Hardware-earned traps (2026-07-04, ESP32-C3 + Pi hub)
- **zenoh-pico has no usrpwd** — `Z_CONFIG_USER/PASSWORD_KEY` exist in its config
  header but no transport code consumes them; a usrpwd-enforcing router rejects the
  session in ~200 ms (loud). MCU identity needs TLS certs or endpoint segregation.
  Rust clients authenticate fine against the same router.
- **WPA2 join fails against the Pi's brcmfmac AP** — 4-way handshake timeout
  (`run → init (0xf00)` loop) despite correct PSK; open AP joins in ~6 s. C3 client
  vs NM/wpa_supplicant AP interop, unresolved — investigate before shipping a
  secured hub-AP.
- **Never `esp_restart()` in GATT write context** — it races the peer's next
  operation (read-back, second write, Improv notify); the client sees "Device
  disconnected" mid-exchange. Fixed: the done-reboot is a 4 s esp_timer one-shot,
  re-armed per completing write and deferred while a client is connected (a
  host-task sleep is not an alternative — it blocks the pending second write).

## zenoh-pico API notes (1.9.0)
- Config: `zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client")` +
  `Z_CONFIG_CONNECT_KEY, "tcp/<ip>:7447"`.
- `z_open(&s, z_move(config), NULL)` — read/lease tasks **auto-start** (multi-thread
  default); do NOT call `zp_start_read_task`/`zp_start_lease_task`.
- Publish: `z_bytes_copy_from_str(&payload, buf)` → `z_publisher_put(z_loan(pub), z_move(payload), NULL)`.
- `z_close` takes a `z_loaned_session_t*` — do NOT call `z_close(z_move(s))` (API
  mismatch); before `esp_restart()` just restart, the OS tears down everything.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` — the 3584 default overflows `z_open` + the loop.

## NimBLE API notes
- `provisioning_advertise(name)` — sets adv fields + starts GAP advertising; owns
  the internal GAP event callback (reconnect after disconnect). Call from `on_sync`.
- `ble_hs_util_ensure_addr(0)` must run before `ble_hs_id_infer_auto` — call in `on_sync`.
- Never init NimBLE in operating mode — one radio path per boot.

## Verification
Verify on a **non-isolated network** (phone hotspot + laptop `hubd` as the router).
A client-isolated campus network (e.g. WhiteSky) blocks rover→hub regardless of
correct code.

## Conventions
- **Measured data only** — publish only what the board truly measures (uptime, heap).
  No faked IMU. `synthetic:false`.
- **NVS namespace `"rover"`** — keys: `ssid`, `pass`, `locator`. Nothing else —
  mode state deliberately does not persist.
- **One radio path per boot** — operating mode: Wi-Fi only; provisioning mode: BLE only.

## Status
v2 **hardware-verified 2026-07-04** — a three-rover fleet against the Pi hub in AP mode:
a044 (classic devkit) · b79c (C3 SuperMini) · c9d0 (ESP32-CAM)
(`hub-0d08`, open router): BLE
provisioning → Wi-Fi → zenoh session → both rovers' telemetry interleaved at one
subscriber; hub outage → 19 s dead-session detection → provisioning window →
window expiry → automatic rejoin, no human touch. Window re-provisioning, the
debounced done-reboot (read-back + linger survive; reboot exactly 4 s after the
last write, post-disconnect), and the BOOT button both directions (window → retry
now at 24.7 s; boot → publishing in 3.2 s) all verified. v3: `led` queryable +
chip temp. Roadmap: `better-robotics/hub#1`.
