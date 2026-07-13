# robot

One ESP32 image, both ends of the wire. Every board is a **rover** — an
[`esp-mqtt`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html)
drive client — and any board can become **the whole hub** (open Wi-Fi AP + MQTT
broker + dashboard) the moment a room needs one. Runtime decisions, never
builds: boards flash identically and get named later. The contract and the
Raspberry Pi hub live at [`better-robotics/hub`](https://github.com/better-robotics/hub).

## How a board decides what to be

```
power on
   │
   ├── sees an open hub-… ────────▶ joins it · drives off its broker
   │                                 (ESP32 hub-XXXX, or Pi hub-pi-…; a stored
   │                                  hub pin narrows this to ONE exact hub)
   │
   └── sees none ─────────────────▶ becomes its own hub — AP + broker +
                                     dashboard at http://rover.local
                                     (stored home Wi-Fi = internet uplink;
                                      drives fine with none)
                                        │
                                        └─⟲ keeps watching: when a hub-…
                                            appears, steps down and joins it
```

The board is always **AP + station at once** — its own open `rover-XXXX`
network never goes away. `http://rover.local` always answers with a landing
page that routes to wherever the dashboard is *right now*, and holds the
**Wi-Fi & role settings** (network scanner, home Wi-Fi, rover/hub/auto switch).

## Life of a board

```
flash ──▶ power on ──▶ unassigned pool ──▶ Blink 💡 ──▶ assigned ──▶ drives as
browser    joins a      on the dashboard;   LED flashes:  team ·      robots/<team>
or pio     hub, or      professor-only      match id to   name ·      until told
           islands      until assigned      desk robot    hub pin ·   otherwise
                                                          motor pins
```

- **Flash from a browser** — [better-robotics.github.io](https://better-robotics.github.io/)
  (desktop Chrome/Edge over USB) — or `pio run -e <env> -t upload`
  (envs: `esp32dev` · `esp32c3-supermini` · `esp32cam` · `rover-l298n`).
- **Zero-touch join**: no stored network → scan, join the strongest open
  `hub-…`, dial its gateway at `mqtt://<gateway>:1883`. Publishing in seconds.
- **Assignment is remote** (dashboard → `cmd/config` → NVS): no per-device
  setup step, no Bluetooth. The **hub pin** locks a board to one exact hub
  SSID so nobody's rogue `hub-…` can absorb it.
- Everything stored is optional — a factory-erased board is fully usable.

## The wire

All topics under `robots/<team>/…` — the team IS the MQTT username, enforced
per-subtree by the Pi broker's ACL. `▲` board publishes · `▼` board obeys:

| topic | | payload |
|---|---|---|
| `sys` | ▲ 2 s | `{"uptime_ms":…,"free_heap":…,"hw":"esp32cam","board":"rover-XXXX","ip":…,"cam":…,"rssi_dbm":…}` — `rssi_dbm` only while the STA uplink is associated |
| `pwm` | ▼ | `{"left_motor":180,"right_motor":-180,"duration_ms":200}` — signed ±255/wheel; a watchdog coasts to a stop `duration_ms` after the last command |
| `cmd/config` | ▼ | assign: `team` `pass` `name` `hub` (pin; `""` clears) `pins` (L298N wiring) — optional `target` board-id addresses one of N |
| `cmd/identify` | ▼ | blink the LED ~6 s — find the physical board |
| `cmd/reprovision` | ▼ | remote reboot |

HTTP on the board itself: `/` state-routing landing · `/wifi` settings ·
`/wifi/status` live state JSON — plus, when hosting a dashboard, `/fleet` and a
`:9001` MQTT-over-WebSocket bridge; the ESP32-CAM streams MJPEG at `:81/stream`.

Motor pins default to the L298N kit (`ENA=25 IN1=26 IN2=27 · ENB=14 IN3=12
IN4=13`) and are re-wireable from the dashboard for a custom chassis.

## Identity & recovery

Two ids, split by job: the **team** (MQTT credential = topic subtree; fresh
boards are `unassigned`, a pool identity only the professor can drive) and
**`rover-XXXX`** (last 2 MAC bytes — the AP name, the `board` telemetry field,
the serial-log token; hardware model is metadata, so boards swap without
identity churn).

Recovery is layered: `esp-mqtt` auto-reconnects through brief outages; a dead
session (~20 s) re-evaluates the whole decision tree above **without
rebooting**; the BOOT button (hold ~1 s) forces a reboot/rescan; and
`cmd/reprovision` is the remote twin (the ESP32-CAM has no button). The onboard
LED = "reached the broker"; a ~6 s blink = someone pressed **Blink**.

## Layout

```
src/
├── main.c               boot dispatcher: role_pref → board_run | hub_role_run
├── hub_role.c           Wi-Fi + broker services — always-APSTA board, tier-2 hub,
│                        discovery + hub-watch (islands yield to real hubs), NAT
├── rover_role.c         drive client — esp-mqtt, motors + watchdog, cmd/* handlers
├── wifi_portal.c        the board's :80 — landing, Wi-Fi & role settings, /wifi/status
├── ws_mqtt_bridge.c     :9001 WebSocket↔MQTT bridge + serves the embedded dashboard
├── camera.c             ESP32-CAM MJPEG (:81)
├── rover_config.c       NVS — network, team identity, motor pins, boot role, hub pin
└── provisioning_util.c  pure helpers: robot id, locator, hub admission (unit-tested)
web/dashboard.html       VENDORED from better-robotics/hub — tools/sync-dashboard.sh --check
tools/                   dashboard embed (pre-build hook) + sync/drift-check
test/test_util/          Unity tests for the pure helpers
```

## Build & test

```sh
pio run -e esp32c3-supermini -t upload   # espressif32@6.13.0 pinned; esp-mqtt is in-tree
pio test -e native                       # Unity: locator, hub admission, id format
pio device monitor                       # 115200 baud
```

The hub role adds two managed components: `espressif/mosquitto` (the on-chip
broker) and `espressif/mdns`.
