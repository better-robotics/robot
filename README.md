# rover

ESP32 rover firmware for the classroom Robotics Hub — a
[`zenoh-pico`](https://github.com/eclipse-zenoh/zenoh-pico) client that publishes
telemetry to (and will serve the `led` RPC from) the Zenoh hub at
[`better-robotics/hub`](https://github.com/better-robotics/hub).

Verified on an ESP32-C3 SuperMini against the Pi hub in AP mode: BLE provisioning →
Wi-Fi → `zenoh-pico` client session to `hubd` → telemetry on `robots/<id>/sys`
delivered to a subscriber — and through a full outage drill: hub down → dead session
detected in ~19 s → BLE provisioning window → window expiry → automatic rejoin,
no human touch. Also runs on the classic ESP32-D0WD (`esp32dev` env).

## Build + flash

```sh
pio run -t upload        # board: az-delivery-devkit-v4 (= classic ESP32), CP2102 auto-reset
```

Watch serial: `pio device monitor` (115200 baud).

PlatformIO is zenoh-pico's official ESP-IDF path — its `library.json` + `extra_script`
compile the library. Pure `idf.py` won't work.

## Provisioning

On first boot (no stored config) the rover enters **BLE provisioning mode** and
advertises as `rover-XXXX` (XXXX = last 2 bytes of the Wi-Fi MAC address, e.g.
`rover-c9d0`).

Provision it with the [Rover setup page](https://better-robotics.github.io/provision/rover.html)
or any Improv-over-BLE client:

1. Connect to `rover-XXXX`.
2. Send Wi-Fi credentials (Improv `SEND_WIFI_SETTINGS` command).
3. Write the Zenoh hub locator (`tcp/<hub-ip>:7447`) to the hubcfg characteristic
   (`4941adfa-0a40-460f-9096-39d1db36f53b`).

Either write can arrive first — the rover reboots into operating mode as soon as
**both** are persisted. On subsequent boots the rover goes straight to operating
mode (no BLE).

## Recovery

Two paths, both leaving stored credentials intact (a new write overwrites them):

- **Automatic (duty cycle).** If the rover can't join Wi-Fi (30 s), can't open a
  Zenoh session, or loses the session (5 consecutive failed publishes), it falls
  back to a **3-minute BLE provisioning window**, then reboots and retries the
  stored credentials — so a transient network or hub outage self-heals with no
  human involved, while a persistently broken rover stays discoverable most of
  every cycle. The window extends while a BLE client is connected.
- **Manual (BOOT button).** Hold the BOOT button (GPIO0) for ~1 second:
  in operating mode → drop into a provisioning window now; in provisioning
  mode → reboot and retry Wi-Fi now.

Mode state never persists: the fallback rides RTC memory that survives a software
restart but not a power cycle, so power-cycling always yields a clean boot against
the stored credentials.

## Operating mode

After provisioning:
- Joins Wi-Fi from NVS credentials.
- Opens a zenoh-pico client session to the hub locator.
- Declares a publisher on `robots/rover-XXXX/sys`.
- Publishes `{"uptime_ms":…,"free_heap":…,"synthetic":false}` every 2 s.

## Identity

Each rover's identity is derived from its Wi-Fi MAC address:
`rover-XXXX` where XXXX is the hex representation of the last 2 MAC bytes.
This appears in the BLE advertisement name, the Zenoh key (`robots/rover-XXXX/sys`),
and serial logs — no hardcoded ID.

## Layout

```
platformio.ini      PlatformIO/ESP-IDF; zenoh-pico via lib_deps (git), espressif32@6.13.0
partitions.csv      Custom 3 MB app partition (BLE + Wi-Fi + zenoh-pico exceed 1 MB default)
CMakeLists.txt      IDF project root
src/main.c          Mode dispatch, duty cycle, BOOT button, operating mode
src/provisioning.c  Improv + hubcfg GATT services; provisioning_advertise()
src/rover_config.c  NVS-backed credentials (ssid, pass, locator)
src/CMakeLists.txt  component REQUIRES
sdkconfig.defaults  Stack size, 4 MB flash, BT/NimBLE enabled
```

## Status

**v2: BLE provisioning** — Improv Wi-Fi + hubcfg locator, MAC-derived identity,
stateless duty-cycle recovery, BOOT-button mode switch. Roadmap: `led` queryable +
hub publisher verification (`better-robotics/hub#1`).
