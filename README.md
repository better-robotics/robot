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
  under the new team. A rover is named *after* it joins, not before — no
  per-device onboarding step, no Bluetooth.

Pointing a rover at a *specific* (non-open) network isn't supported yet — it
returns with hub self-election (a board that finds no hub becomes one and serves
a Wi-Fi picker). Until then a rover joins the open classroom `hub-*` AP.

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

- **Automatic.** Can't join (30 s), no broker in 10 s, or a ~20 s dead session →
  reboot → retry the whole discovery/connect path. A rover powered on before its
  hub just loops, rescanning, until the hub's AP appears; `esp-mqtt`
  auto-reconnects, so brief outages self-heal in place without a reboot.
- **BOOT button (hold ~1 s).** Reboots — force a rescan / recover a wedged rover.
  (The ESP32-CAM has no BOOT button — use the remote path.)
- **Remote.** Publish anything to `robots/<id>/cmd/reprovision` and the rover
  reboots.

The onboard LED lights when the rover reaches the broker — a visible "live and
drivable" signal.

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
  same token in the `board` telemetry field and serial logs. It carries the
  robot's *role*; the hardware model (`esp32-devkit` ·
  `esp32c3-supermini` · `esp32cam`) is metadata, never part of the name — boards
  swap without identity churn.

## Layout

```
platformio.ini      PlatformIO/ESP-IDF; espressif32@6.13.0 (esp-mqtt is in-tree, no lib_deps)
partitions.csv      3 MB app partition (over-large since BLE was removed; harmless)
CMakeLists.txt      IDF project root
src/main.c          Discovery, connect/retry, BOOT button, MQTT client, motor drive
src/provisioning_util.c  Robot-id formatting + locator validation (no BLE — just helpers)
src/rover_config.c  NVS config: network (ssid/pass/locator), team identity, motor pins
src/CMakeLists.txt  component REQUIRES (mqtt, json — no bt)
sdkconfig.defaults  Stack size, 4 MB flash (BLE removed)
```

## Status

**v5: MQTT + drive, BLE removed** — an `esp-mqtt` client (per-team
username/password auth), motor drive from `robots/<id>/pwm`, post-join team +
motor-pin assignment from the hub dashboard, and — new in v5 — the entire BLE
onboarding stack deleted (post-join config made it redundant), freeing ~175 KB
flash and ~44 KB heap. Zero-touch open-`hub-*` scan-join, gateway-derived broker,
remote reboot, three board envs. Hardware-validated on a classic ESP32-D0WD
devkit and ESP32-C3 SuperMini against a live hub. Next: hub self-election (a
board that finds no hub becomes one — also restores specific-network setup),
then the LED RPC.
