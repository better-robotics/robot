# robot

ESP32 firmware for the robot end of the classroom Robotics Hub — a
[`zenoh-pico`](https://github.com/eclipse-zenoh/zenoh-pico) client that publishes
telemetry to (and will serve the `led` RPC from) the Zenoh hub at
[`better-robotics/hub`](https://github.com/better-robotics/hub). Robots are
role-named — every unit today is a rover (`rover-XXXX`); the hardware model rides
telemetry as metadata, so one codebase covers every board, and future roles, without
renaming anything.

Verified on a three-board fleet against the Pi hub in AP mode — classic ESP32-D0WD
devkit, ESP32-C3 SuperMini, AI-Thinker ESP32-CAM — including a full outage drill
(hub down → dead session detected → BLE window → automatic rejoin, no human touch)
and zero-touch onboarding: a factory-erased board reached publishing in under 6 s
with nothing configured.

## Build + flash

```sh
pio run -e <env> -t upload     # envs: esp32dev · esp32c3-supermini · esp32cam
```

Watch serial: `pio device monitor` (115200 baud).

PlatformIO is zenoh-pico's official ESP-IDF path — its `library.json` + `extra_script`
compile the library. Pure `idf.py` won't work.

## Onboarding

A robot needs nothing configured:

- **Zero-touch (default).** With no stored network, it scans for an *open* `hub-…`
  network — the classroom hub's access point — joins the strongest one, and dials
  that network's gateway (on the hub's AP, the hub itself) at `tcp/<gateway>:7447`.
  Power on near a hub and it's on the fabric.
- **BLE provisioning (override).** To point a robot at a specific network or hub,
  use the [Rover setup page](https://better-robotics.github.io/provision/rover.html)
  or any Improv-over-BLE client. While the window is open the onboard LED is lit
  and the robot advertises as `rover-XXXX`:
  1. Send Wi-Fi credentials (Improv `SEND_WIFI_SETTINGS`).
  2. Write the hub locator (`tcp/<hub-ip>:7447`) to the hubcfg characteristic
     (`4941adfa-0a40-460f-9096-39d1db36f53b`).

  Either write can arrive first. Stored values always win over discovery, and a
  Wi-Fi-only write is complete by itself — the locator derives from the gateway.
  Discovery is never persisted: storage holds explicit choices only.

## Recovery

- **Automatic (duty cycle).** Can't join (30 s), can't open a session, or 5
  consecutive failed publishes → a **3-minute BLE provisioning window** (LED on;
  extends while a client is connected) → reboot → retry stored config, or re-scan
  for a hub. A robot powered on before its hub simply alternates scan → window
  until the hub appears.
- **BOOT button (hold ~1 s).** Operating mode → drop into a provisioning window
  now; provisioning mode → reboot and retry now. (The ESP32-CAM has no BOOT
  button — use the remote path.)
- **Remote.** Publish anything to `robots/<id>/cmd/reprovision` and the robot reboots
  into a provisioning window.

Mode state never persists: the fallback rides RTC memory that survives a software
restart but not a power cycle, so power-cycling always yields a clean boot.

## Operating mode

- Joins Wi-Fi (stored network, or a discovered open `hub-…`).
- Opens a zenoh-pico client session to the locator (stored, or gateway-derived).
- Publishes `{"uptime_ms":…,"free_heap":…,"hw":"<board>","synthetic":false}`
  every 2 s on `robots/rover-XXXX/sys`.
- Subscribes to `robots/rover-XXXX/cmd/reprovision`.

## Identity

`rover-XXXX`, where XXXX is the last 2 bytes of the Wi-Fi MAC (e.g. `rover-c9d0`) —
the same token in the BLE advertisement, the Zenoh keys, and serial logs; no
hardcoded ID. The name carries the robot's *role*; the hardware model
(`esp32-devkit` · `esp32c3-supermini` · `esp32cam`) is metadata in telemetry and
Improv device-info, never part of the name — boards can be swapped without
identity churn.

## Layout

```
platformio.ini      PlatformIO/ESP-IDF; zenoh-pico via lib_deps (git), espressif32@6.13.0
partitions.csv      Custom 3 MB app partition (BLE + Wi-Fi + zenoh-pico exceed 1 MB default)
CMakeLists.txt      IDF project root
src/main.c          Mode dispatch, zero-touch discovery, duty cycle, BOOT button, operating mode
src/provisioning.c  Improv + hubcfg GATT services; provisioning_advertise()
src/rover_config.c  NVS-backed explicit config (ssid, pass, locator)
src/CMakeLists.txt  component REQUIRES
sdkconfig.defaults  Stack size, 4 MB flash, BT/NimBLE enabled
```

## Status

**v3: zero-touch fleet** — open `hub-*` scan-join, gateway-derived locator, remote
`reprovision` topic, provisioning LED, hardware metadata, three board envs.
Roadmap: `led` queryable + securing the classroom AP (WPA2) once the Pi↔C3
interop question is settled.
