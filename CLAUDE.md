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
telemetry), never part of a name. Don't "fix" role-prefixed identifiers
(`rover-`, `namePrefix`) to say robot — role vocabulary is product surface and stays.

## Build
- **PlatformIO + ESP-IDF** — `pio run -e <env> [-t upload]`. esp-mqtt is in-tree
  with ESP-IDF (no external lib), so `mqtt` is just a component in `REQUIRES`
  (src/CMakeLists.txt). Both `esp32dev` (xtensa) and `esp32c3-supermini` (riscv)
  build-verified 2026-07-09.
- Three envs: `esp32dev` (classic ESP32-D0WD devkit, CP2102, BOOT=GPIO0),
  `esp32c3-supermini` (ESP32-C3 QFN32, native USB-Serial/JTAG, BOOT=GPIO9 via
  `-DBUTTON_GPIO`), `esp32cam` (AI-Thinker ESP32-CAM — no USB socket, flashed
  via a plug-in USB↔UART adapter; **no BOOT button**, so its only manual
  re-entry is the `reprovision` topic; LED=GPIO33 rear red, active-low). Each env passes `-DMQTT_USER`/`-DMQTT_PASS` (default demo `team1`)
  as a *fallback* only — a rover's team is assigned post-join over MQTT
  (`robots/<id>/cmd/config`, persisted to NVS) from the hub dashboard's "Assign
  a rover" panel, so boards flash identically and get named at the hub.
- Platform pinned **`espressif32@6.13.0`** (IDF 5.1) for reproducible builds —
  the old `<7.x` constraint was zenoh-pico's PIO build; that reason is gone, the
  pin is now just stability.
- **Custom `partitions.csv`** — kept from the zenoh era (it needed 3 MB). The
  MQTT + no-BLE firmware is far leaner (Flash ~29%), so the table is now
  over-large, not tight; harmless to keep.

## Boot flow

One mode, one radio: Wi-Fi STA + esp-mqtt (BLE removed 2026-07-09 — see below).
Boot is a pure function of NVS; no stored state is ever a dead end. **Nothing
stored is fully operable**: no ssid → scan-join the strongest *open* `hub-*`
network (the classroom AP convention is the onboarding channel); no locator →
dial the DHCP gateway (on its own AP the hub is the gateway); no team → the
compile-time demo credential until the dashboard assigns one. Discovery results
are never persisted — stored config is retried first every boot, and when a
stored join fails, a live open `hub-*` is tried before giving up (the stored
locator is ignored on that path: half-stale config isn't trusted by halves).

```
boot ──► Wi-Fi STA: stored ssid, or discover open hub-*
         → mqtt connect(stored locator, or mqtt://<gateway>:1883) as <team>
         → LED on; publish robots/<team>/sys every 2s
         → subscribe robots/<team>/{pwm, cmd/config, cmd/reprovision}
  │ can't join (30s) · no CONNACK in 10s (unreachable OR bad credential)
  │ · ~20s dead session · button · reprovision message
  ▼
esp_restart() → retry the whole path   ← pre-hub power-on just loops here,
                                         rescanning, until the hub appears
```
esp-mqtt **auto-reconnects**, so a brief hub outage self-heals in place without a
reboot; only a sustained (~20 s) dead session forces a restart.

**BLE removed (2026-07-09, #11).** Post-join config replaced BLE onboarding, so
the offline provisioning window (NimBLE + Improv + the `hubcfg` characteristic)
had no job left — deleted, freeing ~175 KB flash + ~44 KB heap. The one case it
uniquely covered (pointing a rover at a *specific* network) returns with hub
self-election, its intended replacement. Mode dispatch, the RTC provision-request
flag, and "one radio path per boot" all went with it — there's one path now.

**BOOT button (GPIO0, hold ~1 s):** reboot — force a rescan / recover a wedged
rover. **Remote twin:** publish anything to `robots/<id>/cmd/reprovision` — the
ESP32-CAM's only re-entry besides join failure (no button). **LED:** on = reached
the broker (a visible "live and drivable" signal; was the provisioning-window LED).

**Broker discovery = the DHCP gateway** — on the hub's own AP the gateway IS the
hub, so `mqtt://<gateway>:1883` reaches the broker with no name lookup and no
hardcoded IP (the one address the Pi and ESP32 hubs don't share). A stored
locator overrides. No multicast — campus Wi-Fi filters it and isolates clients.

## Identity
Two ids, split by job (CONTRACT.md § Discovery & isolation):
- **Topic/auth id == the team.** The rover authenticates as its team credential
  and publishes under `robots/<team>/*`, so the Pi's `pattern robots/%u/#` ACL
  admits it and teams can't cross. Compile-time demo `team1` (`-DMQTT_USER`) is
  the fallback; the real team is assigned post-join from the dashboard
  (`robots/<id>/cmd/config` → NVS).
- **`rover-XXXX`** (last 2 MAC bytes via `rover_format_robot_id`) is a `board`
  field in the sys payload — hardware is metadata, never the topic id.

## Hardware-earned traps (2026-07-04, ESP32-C3 + Pi hub)
- **~~zenoh-pico has no usrpwd~~ → RESOLVED by the MQTT port (2026-07-09).** This
  was the deciding scar: zenoh-pico declares `Z_CONFIG_USER/PASSWORD_KEY` but no
  transport code consumes them, so a per-team MCU identity was impossible (a
  usrpwd router rejected the session in ~200 ms). esp-mqtt authenticates with
  username/password natively — the reason the rover ships on MQTT, not Zenoh.
- **WPA2 join fails against the Pi's brcmfmac AP** — 4-way handshake timeout
  (`run → init (0xf00)` loop) despite correct PSK; open AP joins in ~6 s. C3 client
  vs NM/wpa_supplicant AP interop, unresolved — investigate before shipping a
  secured hub-AP. (Still live: self-election's config page must handle it.)
- *(Retired 2026-07-09 with the BLE removal: the "never `esp_restart()` in GATT
  write context" scar — no GATT context can recur now that NimBLE is gone.)*

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

## Verification
Verify on a **non-isolated network** (phone hotspot + laptop `hubd` as the router).
A client-isolated campus network (e.g. WhiteSky) blocks rover→hub regardless of
correct code.

## Conventions
- **Measured data only** — publish only what the board truly measures (uptime, heap).
  No faked IMU. `synthetic:false`.
- **NVS namespace `"rover"`** — keys: `ssid`/`pass`/`locator` (network),
  `user`/`mpass`/`name` (post-join team identity), `mpins` (6-byte motor-pin
  blob, a custom-wired chassis). All optional — absent falls back to the
  compile-time default.

## Status
**v5 (MQTT + drive, BLE removed) — hardware-validated 2026-07-09.** The rover is
an esp-mqtt client with per-team auth, drives an L298N from `robots/<id>/pwm`,
takes its team + motor pins post-join from the dashboard (`cmd/config` → NVS),
and the entire BLE stack is gone (#11 — post-join config made it redundant),
freeing ~175 KB flash + ~44 KB heap. Validated on rover-a044 (ESP32-D0WD) and
rover-b79c (ESP32-C3) against a live hub: boot → join open `hub-*` → connect as
the NVS-stored team → publish + drive; motor init, LED-on-connect, and NVS
identity persistence across reflash all confirmed.

**Next: hub self-election** — a board that finds no `hub-*` becomes one (AP +
on-chip broker) and serves a Wi-Fi picker, which also restores the
specific-network setup BLE used to do. See `hub/esp32/DESIGN-unified.md`.

History (git, pre-2026-07-09): v2 zenoh firmware (three-rover fleet, BLE
provisioning, dead-session self-heal); v3/v4 the MQTT port + motor drive.
