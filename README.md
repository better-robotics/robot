# robot

One ESP32 image, both ends of the wire. Every board is a **robot** — a
[`zenoh-pico`](https://zenoh.io/) drive client — and any board can become **the
whole hub** (open Wi-Fi AP + Zenoh endpoint + dashboard) the moment a room needs
one. Runtime decisions, never builds: boards flash identically and get named
later. The contract and the Raspberry Pi hub live at
[`sprocket-robotics/hub`](https://github.com/sprocket-robotics/hub).

## How a board decides what to be

```
power on
   │
   ├── sees an open hub-… ────────▶ joins it · drives off its Zenoh hub ·
   │                                 shuts its OWN network off — one
   │                                 network in the room, the hub's
   │                                 (ESP32 hub-XXXX, or Pi hub-pi-…; a stored
   │                                  hub pin narrows this to ONE exact hub)
   │
   └── sees none ─────────────────▶ becomes its own hub — AP + Zenoh +
                                     dashboard at http://robot.local
                                     (stored home Wi-Fi = internet uplink;
                                      drives fine with none)
                                        │
                                        └─⟲ keeps watching: when a hub-…
                                            appears, steps down and joins it
```

A board comes up as **AP + station at once**, then follows the room:

- **On its own** it keeps its open `robot-XXXX` network up. `http://robot.local`
  answers with a landing page that routes to wherever the dashboard is *right
  now*, and holds the **Wi-Fi & role settings** (network scanner, home Wi-Fi,
  robot/hub/auto switch).
- **Joined to a hub** it shuts that network down. The hub is the one place to
  connect: its Wi-Fi, its dashboard, its Zenoh session. A robot on a hub is just a
  robot — it isn't also a network to get lost in, and it isn't spending the
  hub's airtime advertising itself. Its settings page follows it, at
  `http://robot-XXXX.local` on the hub's network.

Every board answers at its own **`robot-XXXX.local`** wherever it is. The short
`robot.local` is a convenience on a board's own network, where it's the only
one answering.

## Life of a board

```
flash ──▶ power on ──▶ unassigned pool ──▶ Blink 💡 ──▶ assigned ──▶ drives as
browser    joins a      on the dashboard;   LED flashes:  name ·      robots/<name>
or pio     hub, or      anyone on the hub's match id to   hub pin ·   until told
           islands      Wi-Fi can drive it   desk robot    motor pins  otherwise
```

- **Flash from a browser** — [sprocket-robotics.github.io](https://sprocket-robotics.github.io/)
  (desktop Chrome/Edge over USB) — or `pio run -e <env> -t upload`
  (envs: `esp32dev` · `esp32c3-supermini` · `esp32cam` · `robot-l298n`).
- **Update over Wi-Fi after that** — a freshly flashed board never needs the
  cable again:

  ```
  OPERATOR_PASS=… tools/ota-push.py --host robot-XXXX.local \
      .pio/build/<env>/firmware.bin
  ```

  Each board holds two firmware slots and boots a pushed image on trial: an
  update that doesn't come up is reverted on its own, with no cable. A push that
  fails — wrong password, unreachable board, corrupt image — leaves the board
  running exactly what it was running before. What this can't catch is an update
  that boots fine and is simply wrong: the board checks that it came up, not
  that it works.

  `OPERATOR_PASS` is the board's operator password — the same one the hub's
  fleet controls need. It's `change-me` until you set one on the board's own
  config page (`http://robot-XXXX.local/wifi` → **Operator password**).

  **Boards flashed before OTA existed need one more USB pass**, to pick up the
  two-slot partition table; until then they answer a push with *"no OTA slot on
  this board"*. Re-check their Wi-Fi and name afterwards — the new table
  reclaims a slice of the config area, so stored settings may not survive.
- **Zero-touch join**: no stored network → scan, join the strongest open
  `hub-…`, reach its gateway's Zenoh endpoint at `tcp/<gateway>:7447`. Publishing in seconds.
- **Assignment is remote** (dashboard → `cmd/config` → NVS): no per-device
  setup step, no Bluetooth. The **hub pin** locks a board to one exact hub
  SSID so nobody's rogue `hub-…` can absorb it.
- Everything stored is optional — a factory-erased board is fully usable.

## The wire

All topics under `robots/<name>/…` — `<name>` is a topic address, not a
credential: the hub's own Wi-Fi is the real boundary, and every robot/browser
gets full read+write with no auth at all (the hub's Wi-Fi perimeter is the boundary). `▲` board
publishes · `▼` board obeys:

| topic | | payload |
|---|---|---|
| `sys` | ▲ 2 s | `{"uptime_ms":…,"free_heap":…,"hw":"esp32cam","board":"robot-XXXX","ip":…,"cam":…,"rssi_dbm":…}` — `rssi_dbm` only while the STA uplink is associated |
| `pwm` | ▼ | `{"left_motor":180,"right_motor":-180,"duration_ms":200}` — signed ±255/wheel, positive = forward; *left* = the robot's own left, standing behind it facing forward; a watchdog coasts to a stop `duration_ms` after the last command |
| `cmd/config` | ▼ | assign: `name` `hub` (pin; `""` clears) `pins` (L298N wiring) — no password field; optional `target` board-id addresses one of N |
| `cmd/identify` | ▼ | blink the LED ~6 s — find the physical board |
| `cmd/reprovision` | ▼ | remote reboot |

HTTP on the board itself: `/` state-routing landing · `/wifi` settings ·
`/wifi/status` live state JSON — plus, when hosting a dashboard, `/fleet` and a
`:9001` WS-JSON adapter (browser ↔ the hub's Zenoh session); the ESP32-CAM streams MJPEG at `:81/stream`.

Motor pins default to the L298N kit (`ENA=25 IN1=26 IN2=27 · ENB=14 IN3=12
IN4=13`; C3 SuperMini: `ENA=6 IN1=0 IN2=1 · ENB=5 IN3=3 IN4=4`) and are
re-wireable from the dashboard for a custom chassis. **Wiring convention:** the
**left** wheel — the robot's left, standing behind it — plugs into the L298N's
**OUT1/OUT2** (channel A); firmware pin names match the silkscreen 1:1. If a
robot mirrors its turns while forward/reverse work, the motor plugs are
swapped: confirm with one wheel at a time
(`{"left_motor":150,"right_motor":0,"duration_ms":1500}` must spin the left
wheel forward) and swap the plugs rather than remapping pins.

## Identity & recovery

Two ids, split by job: the **name** (a topic address, not a credential — fresh
boards are `unassigned`, a pool name anyone on the hub's Wi-Fi can drive; it
may be driven by one student or a few sharing the board, the protocol has no
notion of team size, only whoever's on the hub's Wi-Fi drives it) and
**`robot-XXXX`** (last 2 MAC bytes — the AP name, the `board` telemetry field,
the serial-log token; hardware model is metadata, so boards swap without
identity churn).

Recovery is layered: zenoh-pico does **not** auto-reconnect in place — a dead
session (~20 s of failed publishes) returns to the boot loop, which re-evaluates
the whole decision tree above **without rebooting**; the BOOT button (hold ~1 s)
forces a reboot/rescan; and `cmd/reprovision` is the remote twin (the ESP32-CAM
has no button). The onboard LED = "reached the hub"; a ~6 s blink = someone
pressed **Blink**.

## Layout

```
src/
├── main.c               boot dispatcher: role_pref → board_run | hub_role_run
├── hub_role.c           Wi-Fi + Zenoh hub services — the board (AP until a hub takes
│                        over), tier-2 hub, discovery + hub-watch (islands yield
│                        to real hubs), NAT
├── robot_role.c         drive client — zenoh-pico, motors + watchdog, cmd/* handlers
├── wifi_portal.c        the board's :80 — landing, Wi-Fi & role settings, /wifi/status
├── ws_zenoh_bridge.c    :9001 WS-JSON adapter (browser ↔ Zenoh) + serves the embedded dashboard
├── camera.c             ESP32-CAM MJPEG (:81)
├── robot_config.c       NVS — network, name identity, motor pins, boot role, hub pin
└── provisioning_util.c  pure helpers: robot id, locator, hub admission (unit-tested)
web/dashboard.html       VENDORED from sprocket-robotics/hub — tools/sync-dashboard.sh --check
tools/                   dashboard embed (pre-build hook) + sync/drift-check
test/test_util/          Unity tests for the pure helpers
```

## Build & test

```sh
pio run -e esp32c3-supermini -t upload   # espressif32@6.13.0 pinned; zenoh-pico via lib_deps
pio test -e native                       # Unity: locator, hub admission, id format
pio device monitor                       # 115200 baud
```

The Zenoh transport is a `lib_deps` git pin (`zenoh-pico`); the hub role adds
`espressif/mdns` as a managed component (advertises `hub.local`).
