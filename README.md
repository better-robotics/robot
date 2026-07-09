# robot

ESP32 firmware for the robot end of the classroom Robotics Hub — an
[`esp-mqtt`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html)
client that publishes telemetry to, and drives from, the hub broker at
[`better-robotics/hub`](https://github.com/better-robotics/hub) (Mosquitto on a
Raspberry Pi, or an on-chip broker on an ESP32 hub — one firmware reaches
either). Robots are role-named — every unit today is a rover (`rover-XXXX`); the
hardware model rides telemetry as metadata, so one codebase covers every board,
and future roles, without renaming anything.

## Build + flash

```sh
pio run -e <env> -t upload     # envs: esp32dev · esp32c3-supermini · esp32cam
```

No toolchain to install? Flash from a browser instead:
**[better-robotics.github.io/provision/flash](https://better-robotics.github.io/provision/flash/)**
(desktop Chrome/Edge, over USB). Watch serial with `pio device monitor` (115200 baud).

`esp-mqtt` ships inside ESP-IDF, so there's no external MQTT library — it's just a
component in `REQUIRES`. Platform pinned to `espressif32@6.13.0`.

## Onboarding

Turn a rover on — that's it.

- **Zero-touch (default).** With no stored network, it scans for an *open*
  `hub-…` network — the classroom hub's access point — joins the strongest one,
  and dials that network's gateway (on the hub's AP, the hub itself) at
  `mqtt://<gateway>:1883`. It comes up under a default demo team, publishing
  within seconds.
- **Assign it from the hub dashboard.** Once a rover is online, the hub
  dashboard's **"Assign a rover"** panel lists it by its board id. Give it a
  **team** (its credential + topic), an optional **name**, and — for a
  student-wired chassis — its **motor pins**. The rover saves them and reconnects
  under the new team. This replaced per-device BLE onboarding: a rover is named
  *after* it joins, not before.
- **BLE (specific/secured network only).** To point a rover at a *particular*
  network instead of the open classroom AP, use the
  [Rover setup page](https://better-robotics.github.io/provision/rover.html) or
  any Improv-over-BLE client. While the window is open the onboard LED is lit and
  the rover advertises as `rover-XXXX`. Stored values win over discovery; a
  Wi-Fi-only write is complete by itself (the broker derives from the gateway).

## Drive

The rover subscribes to `robots/<team>/pwm` and drives an L298N H-bridge:

```json
{ "left_motor": 180, "right_motor": -180, "duration_ms": 200 }
```

Signed ±255 per wheel — sign sets direction, magnitude sets speed. A watchdog
stops the motors `duration_ms` after the last command, so a dropped connection or
a silent controller coasts to a halt instead of running away. Pins default to the
L298N kit's wiring (`ENA=25 IN1=26 IN2=27 · ENB=14 IN3=12 IN4=13`) and are
reconfigurable from the dashboard for a custom chassis.

## Recovery

- **Automatic (duty cycle).** Can't join (30 s), no broker in 10 s, or a ~20 s
  dead session → a **3-minute BLE provisioning window** (LED on; extends while a
  client is connected) → reboot → retry stored config, or re-scan for a hub. A
  rover powered on before its hub simply alternates scan → window until the hub
  appears; `esp-mqtt` auto-reconnects, so brief outages self-heal in place.
- **BOOT button (hold ~1 s).** Operating → drop into a provisioning window now;
  provisioning → reboot and retry now. (The ESP32-CAM has no BOOT button — use
  the remote path.)
- **Remote.** Publish anything to `robots/<id>/cmd/reprovision` and the rover
  reboots into a provisioning window.

Mode state never persists: the fallback rides RTC memory that survives a software
restart but not a power cycle, so power-cycling always yields a clean boot.

## Operating mode

- Joins Wi-Fi (stored network, or a discovered open `hub-…`).
- Connects to the broker (stored locator, or `mqtt://<gateway>:1883`),
  authenticating as its **team** username/password.
- Publishes `{"uptime_ms":…,"free_heap":…,"hw":"<board>","board":"rover-XXXX",…}`
  every 2 s on `robots/<team>/sys`.
- Subscribes to `robots/<team>/pwm` (drive), `/cmd/config` (post-join
  assignment), and `/cmd/reprovision`.

## Identity

Two ids, split by job:

- **Team** — the MQTT username/password. The rover publishes under
  `robots/<team>/*`, and the broker's per-team ACL keeps teams from crossing.
  Assigned post-join from the dashboard (default demo `team1` until then).
- **`rover-XXXX`** — the last 2 bytes of the Wi-Fi MAC (e.g. `rover-c9d0`), the
  same token in the BLE advertisement, the `board` telemetry field, and serial
  logs. It carries the robot's *role*; the hardware model (`esp32-devkit` ·
  `esp32c3-supermini` · `esp32cam`) is metadata, never part of the name — boards
  swap without identity churn.

## Layout

```
platformio.ini      PlatformIO/ESP-IDF; espressif32@6.13.0 (esp-mqtt is in-tree, no lib_deps)
partitions.csv      3 MB app partition (roomy for esp-mqtt; kept from the BLE+Wi-Fi era)
CMakeLists.txt      IDF project root
src/main.c          Mode dispatch, discovery, duty cycle, BOOT button, MQTT client, motor drive
src/provisioning.c  Improv + hubcfg GATT services (network provisioning); provisioning_advertise()
src/rover_config.c  NVS config: network (ssid/pass/locator), team identity, motor pins
src/CMakeLists.txt  component REQUIRES (adds mqtt, json)
sdkconfig.defaults  Stack size, 4 MB flash, BT/NimBLE enabled
```

## Status

**v4: MQTT + drive** — ported from zenoh-pico to `esp-mqtt` (per-team
username/password auth), motor drive from `robots/<id>/pwm`, and post-join team +
motor-pin assignment from the hub dashboard. Zero-touch open-`hub-*` scan-join,
gateway-derived broker, remote `reprovision`, provisioning LED, three board envs
carry over. Hardware-validated on a classic ESP32-D0WD devkit and ESP32-C3
SuperMini against a live hub. Next: LED RPC, securing the classroom AP (WPA2)
once the Pi↔C3 join interop is settled.
