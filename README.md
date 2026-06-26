# rover

ESP32 rover firmware for the classroom Robotics Hub — a
[`zenoh-pico`](https://github.com/eclipse-zenoh/zenoh-pico) client that publishes
telemetry to (and will serve the `led` RPC from) the Zenoh hub at
[`better-robotics/hub`](https://github.com/better-robotics/hub).

Verified on a classic ESP32-D0WD (CP2102 DevKit): joins WiFi → opens a `zenoh-pico`
**client** session to `hubd` over TCP → publishes real chip telemetry on
`robots/<id>/sys`, received live by a `watch` subscriber on the hub side.

## Build + flash

```sh
pio run -t upload        # board: az-delivery-devkit-v4 (= classic ESP32), CP2102 auto-reset
```
Watch serial: `esp32loop watch esp32_devkit` (or `pio device monitor`).

PlatformIO is zenoh-pico's official ESP-IDF path — its `library.json` + `extra_script`
compile the library. Pure `idf.py` won't work: zenoh-pico has no `idf_component_register`,
so it isn't a drop-in IDF component.

## Configure for your network

`src/main.c` hardcodes the WiFi SSID (`DukeVisitor`, open auth) and the hub locator
(`tcp/<ip>:7447`). **Set these for your network before flashing.** Parameterizing them
(build flags / BLE provisioning) is on the roadmap — see `better-robotics/hub#1`.

## Status

**v1: publish-only** — real `uptime_ms` + `free_heap` on `robots/<id>/sys`
(`synthetic:false`; only what the board truly measures — no faked IMU, this board has none).
**v2 (next):** the `led` queryable (`set_led` → real GPIO) + chip temperature. Roadmap in
`better-robotics/hub#1`.

## Layout

```
platformio.ini      PlatformIO/ESP-IDF; zenoh-pico via lib_deps (git), espressif32@6.13.0
CMakeLists.txt      IDF project root
src/main.c          WiFi STA → zenoh client → publish sys telemetry
src/CMakeLists.txt  component REQUIRES (nvs_flash esp_wifi esp_netif esp_event esp_timer)
sdkconfig.defaults  bigger main-task stack, 4 MB flash, large-app partition
```
