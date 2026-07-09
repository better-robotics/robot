# robot — project context

ESP32 firmware: an `esp-mqtt` client for the classroom Robotics Hub
([`better-robotics/hub`](https://github.com/better-robotics/hub)). Publishes
telemetry to the hub broker, drives from `robots/<id>/pwm`, will serve the
`led` RPC. Sibling layer to the hub — the device end, C/ESP-IDF, against the
broker (Mosquitto on the Pi, or on-chip on the ESP32 hub; one firmware reaches
either — both are raw-TCP brokers on :1883, CONTRACT.md § Discovery & isolation).

**Transport: MQTT, ported from zenoh-pico 2026-07-09.** MQTT won the bake-off
(hub-zenoh archived); the deciding factor for *this* firmware was auth —
zenoh-pico has no usrpwd, so a per-team rover identity was impossible; esp-mqtt
authenticates with username/password, which is the whole classroom isolation
model. See git history for the zenoh-era firmware.

**Naming** (repo renamed `rover`→`robot` 2026-07-04): the repo covers any MCU node
role; robots are *role-named* — `rover-XXXX` is today's only role (a future camera
role would be `cam-XXXX`, same codebase). Hardware model is metadata (`hw` in
telemetry + Improv device-info), never part of a name. Don't "fix" role-prefixed
identifiers (`rover-`, `rover.html`, `namePrefix`) to say robot — role vocabulary
is product surface and stays.

## Build
- **PlatformIO + ESP-IDF** — `pio run -e <env> [-t upload]`. esp-mqtt is in-tree
  with ESP-IDF (no external lib), so `mqtt` is just a component in `REQUIRES`
  (src/CMakeLists.txt). Both `esp32dev` (xtensa) and `esp32c3-supermini` (riscv)
  build-verified 2026-07-09.
- Three envs: `esp32dev` (classic ESP32-D0WD devkit, CP2102, BOOT=GPIO0),
  `esp32c3-supermini` (ESP32-C3 QFN32, native USB-Serial/JTAG, BOOT=GPIO9 via
  `-DBUTTON_GPIO`), `esp32cam` (AI-Thinker ESP32-CAM — no USB socket, flashed
  via a plug-in USB↔UART adapter; **no BOOT button**, so provisioning re-entry
  is the join-failure fallback or the `reprovision` topic; LED=GPIO33 rear red,
  active-low). Each env passes `-DMQTT_USER`/`-DMQTT_PASS` (default demo `team1`)
  — override per board to flash different teams until BLE provisioning of creds
  lands.
- Platform pinned **`espressif32@6.13.0`** (IDF 5.1) for reproducible builds —
  the old `<7.x` constraint was zenoh-pico's PIO build; that reason is gone, the
  pin is now just stability.
- **Custom `partitions.csv`** — kept from the zenoh era (it needed 3 MB). esp-mqtt
  is far leaner (Flash ~34%), so the table is now roomy, not tight; harmless to keep.

## Boot flow

Mode dispatch is **stateless**: NVS holds only explicit choices; behavior is a
pure function of that config plus a one-shot provision request in RTC noinit
RAM (survives `esp_restart()`, not a power cycle — no stale mode state can
strand a robot). One radio path per boot. **Nothing stored is fully operable**:
no ssid → scan-join the strongest *open* `hub-*` network (the classroom AP
convention is the onboarding channel); no locator → dial the DHCP gateway
(on its own AP the hub is the gateway). Discovery results are never persisted —
stored config is retried first every boot, and when a stored join fails, a live
open `hub-*` is tried before giving up (the stored locator is ignored on that
path: half-stale config isn't trusted by halves).

```
boot ──► operating mode: stored ssid (or discover hub-*) → Wi-Fi STA
         → mqtt connect(stored locator, or mqtt://<gateway>:1883) as <team>
         → publish robots/<team>/sys every 2s
         → subscribe robots/<team>/cmd/reprovision
  │ nothing to join · join fails (30s) · no CONNACK in 10s (unreachable OR
  │ credential rejected) · ~20s dead session · button · reprovision message
  ▼
provisioning window (BLE, 3 min; extends while a client is connected)
  │ window expires (or button)
  ▼
esp_restart() → operating mode again      ← outages and pre-hub power-on
                                            self-heal by alternating
```
esp-mqtt **auto-reconnects**, so a brief hub outage self-heals in place; only a
sustained (~20 s) dead session drops to a provisioning window.

**BOOT button (GPIO0, hold ~1 s):** operating → provisioning window;
provisioning → retry Wi-Fi now. The human within arm's reach is the out-of-band
channel — this is its API (covers wedged states the firmware can't self-detect).
**Remote twin:** publish anything to `robots/<id>/cmd/reprovision` — the only
re-entry an ESP32-CAM has besides join failure (no button).

**Broker discovery = the DHCP gateway** — on the hub's own AP the gateway IS the
hub, so `mqtt://<gateway>:1883` reaches the broker with no name lookup and no
hardcoded IP (the one address the Pi and ESP32 hubs don't share). A stored
locator overrides. No multicast — campus Wi-Fi filters it and isolates clients.

## Identity
Two ids, split by job (CONTRACT.md § Discovery & isolation):
- **Topic/auth id == the team.** The rover authenticates as its team credential
  and publishes under `robots/<team>/*`, so the Pi's `pattern robots/%u/#` ACL
  admits it and teams can't cross. Currently a compile-time demo `team1`
  (`-DMQTT_USER`); BLE provisioning of per-team creds is the next step.
- **`rover-XXXX`** (last 2 MAC bytes via `rover_format_robot_id`) stays the BLE
  advertising name + a `board` field in the sys payload — hardware is metadata,
  never the topic id.

## Hardware-earned traps (2026-07-04, ESP32-C3 + Pi hub)
- **~~zenoh-pico has no usrpwd~~ → RESOLVED by the MQTT port (2026-07-09).** This
  was the deciding scar: zenoh-pico declares `Z_CONFIG_USER/PASSWORD_KEY` but no
  transport code consumes them, so a per-team MCU identity was impossible (a
  usrpwd router rejected the session in ~200 ms). esp-mqtt authenticates with
  username/password natively — the reason the rover ships on MQTT, not Zenoh.
- **WPA2 join fails against the Pi's brcmfmac AP** — 4-way handshake timeout
  (`run → init (0xf00)` loop) despite correct PSK; open AP joins in ~6 s. C3 client
  vs NM/wpa_supplicant AP interop, unresolved — investigate before shipping a
  secured hub-AP.
- **Never `esp_restart()` in GATT write context** — it races the peer's next
  operation (read-back, second write, Improv notify); the client sees "Device
  disconnected" mid-exchange. Fixed: the done-reboot is a 4 s esp_timer one-shot,
  re-armed per completing write and deferred while a client is connected (a
  host-task sleep is not an alternative — it blocks the pending second write).

## esp-mqtt API notes (ESP-IDF 5.1)
- Config: `esp_mqtt_client_config_t{ .broker.address.uri = "mqtt://<ip>:1883",
  .credentials.username, .credentials.authentication.password,
  .session.keepalive }`.
- `esp_mqtt_client_init` → `esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID,
  cb, NULL)` → `esp_mqtt_client_start`. The client owns its own task; no manual
  read/lease tasks (that was zenoh).
- Auth + reachability both surface as **`MQTT_EVENT_CONNECTED`** — a rejected
  password never reaches it (disconnects first), so "no CONNECT in 10 s" covers
  both unreachable-broker and bad-credential in one gate.
- Publish `esp_mqtt_client_publish(cli, topic, payload, 0, qos, retain)` (len 0 =
  strlen); telemetry is qos 0, no retain. Subscribe in `MQTT_EVENT_CONNECTED`,
  re-fires on every reconnect automatically.
- Before `esp_restart()` just restart — the OS tears the client down.

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
**v3 (MQTT port) — build-verified 2026-07-09, hardware validation pending.**
`esp32dev` + `esp32c3-supermini` both build clean; the whole zenoh transport
swapped for esp-mqtt with per-team username/password auth (main.c). Not yet run
on a board (none connected at port time). Validation gate: flash a rover, join
the hub AP, confirm `robots/team1/sys` lands at a subscriber and a wrong password
is refused — then wire motor drive from `robots/<id>/pwm` (hub#1).

The v2 **zenoh** firmware was hardware-verified 2026-07-04 (three-rover fleet vs
the Pi hub — BLE provision → Wi-Fi → session → interleaved telemetry, 19 s
dead-session self-heal, debounced done-reboot, BOOT button both directions). The
transport-independent halves of that — provisioning, Wi-Fi join/scan, self-heal
alternation, the BOOT button — carry forward unchanged; only the session layer
changed. Full detail in git history (pre-2026-07-09).
