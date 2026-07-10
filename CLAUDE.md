# robot — project context

ESP32 firmware for the classroom Robotics Hub
([`better-robotics/hub`](https://github.com/better-robotics/hub)). **One unified
image, always-APSTA** (self-election collapsed into an always-on AP+STA board
2026-07-09 — see below):

- **`board_run`** (`hub_role.c`, the norm): every board comes up **APSTA** — its
  own open `rover-<id>` AP *and* an STA uplink — and stays that way. It's an
  `esp-mqtt` client that drives an L298N from `robots/<id>/pwm` and takes its team
  + pins post-join, dialing whichever broker is reachable: a discovered `hub-*`'s
  (classroom) or its **own on-chip broker** at `127.0.0.1` (home/island). One
  image reaches either — both are raw-TCP brokers on :1883 (CONTRACT.md §
  Discovery & isolation).
- **`hub_role_run`** (`hub_role.c`): the *whole* on-chip hub — AP+STA+NAPT +
  Mosquitto broker + WS bridge + served dashboard — as a **dedicated** tier-2 hub
  (`role_pref=HUB`, `hub-*` AP, no drive). Folded in from `better-robotics/hub/esp32`.
  The island path reuses these same services against a `rover-<id>` AP.

`src/main.c` dispatches on `role_pref`; `hub_role.c` owns the Wi-Fi + broker
(`board_run` + `hub_role_run`), `rover_role.c` is the **drive client**
(`rover_client_run` + motors, no Wi-Fi of its own). **The ESP hub's contract
sources — `dashboard.html`, CONTRACT — stay canonical in the `hub` monorepo**;
this repo vendors `web/dashboard.html` (drift-checked, `tools/sync-dashboard.sh`).

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
  with ESP-IDF; the **hub role** adds two managed components (`src/idf_component.yml`:
  `espressif/mosquitto` = the on-chip broker, `espressif/mdns`). Unified image
  build-verified 2026-07-09 on both `esp32dev` (xtensa, 48% flash) and
  `esp32c3-supermini` (riscv, 51%) — ~1.5 MB of the 3 MB factory partition.
- Envs: `esp32dev` (classic ESP32-D0WD devkit, CP2102, BOOT=GPIO0),
  `esp32c3-supermini` (ESP32-C3 QFN32, native USB-Serial/JTAG, BOOT=GPIO9 via
  `-DBUTTON_GPIO`), `esp32cam` (AI-Thinker ESP32-CAM — no USB socket, flashed
  via a plug-in USB↔UART adapter; **no BOOT button**, so its only manual
  re-entry is the `reprovision` topic; LED=GPIO33 rear red, active-low),
  `rover-l298n` (extends esp32dev). Each passes `-DMQTT_USER`/`-DMQTT_PASS`
  (the `unassigned` pool identity) as a *fallback* only — a rover's team is assigned post-join over
  MQTT (`robots/<id>/cmd/config` → NVS) from the dashboard's "Assign a rover"
  panel, so boards flash identically and get named at the hub.
- Platform pinned **`espressif32@6.13.0`** (ships **IDF 5.5.3**, not 5.1 as an
  earlier note claimed) for reproducible builds — the old `<7.x` constraint was
  zenoh-pico's; the pin is now just stability (and 5.5.x is what the mosquitto
  component resolves against).
- **Dashboard embed (hub role):** PlatformIO's SCons build does **not** wire an
  objcopy/`.S` embed into the link — `EMBED_TXTFILES`, `target_add_binary_data`,
  and `board_build.embed_txtfiles` all fail (missing `.S`, or generated-but-not-linked).
  So `tools/embed_dashboard.py` (a **pre-build** `extra_scripts` hook; uses
  `$PROJECT_DIR`, not `__file__`, which SCons doesn't define) generates
  `src/dashboard_html.c` — the dashboard as a plain byte array — from
  `web/dashboard.html`, compiled as an ordinary source. `web/dashboard.html` is a
  VENDORED copy (canonical in the `hub` monorepo); `tools/sync-dashboard.sh`
  resyncs it and `--check` gates drift. `src/dashboard_html.c` is gitignored.
- **Custom `partitions.csv`** (3 MB factory) — the unified image genuinely needs
  it now (~48–51% used); the "over-large, harmless" note from the lean rover-only
  era no longer applies.
- **`src/wifi_creds.h`** (gitignored) — the hub role's STA uplink credentials;
  copy `src/wifi_creds.example.h`. NEVER commit the real one.

## Boot flow

**Dispatcher first (`src/main.c`).** Shared init (NVS) runs once, then
`role_pref` (NVS key `role`, § Identity) picks the path and calls it (neither
returns):
```
app_main ─► role_pref == HUB → hub_role_run()   (tier 2: dedicated hub-* + broker + NAT; no drive)
            else             → board_run(self_broker_ok = role==AUTO)
                                 always-APSTA board: own OPEN rover-<id> AP + STA,
                                 loops (NO reboot): join hub-* → drive its broker;
                                 else AUTO → local broker, drive 127.0.0.1 (island);
                                 else ROVER-pinned → rescan, never island.
```
**Always-APSTA, no mode-switch reboot** (DESIGN-unified.md § Direction change,
2026-07-09). The board is APSTA from line one and never switches radio mode, so
home↔classroom is a **runtime re-point** of the broker URI, not a boot role — the
old `role_boot_as_hub`/`RTC_NOINIT` claim-by-reboot is **deleted** (it only ever
existed to dodge a live STA→APSTA switch; it also caused two HW bugs — the
`RTC_DATA` wipe loop and the pi-watch stack panic). **Islands, not attraction:** a
self-broker board's AP is `rover-<id>`, *not* `hub-*`, so nothing joins it. A
shared broker (central control) is opt-in via an explicit hub (a Pi, or a board
pinned to `role_pref=HUB`). An island board yields to any **`hub-*`** via a
**clean restart** (board_run re-runs, discovers the hub, joins it) — safe against
peer islands, which advertise `rover-<id>` not `hub-*`, so it also self-heals a
missed boot scan and joins a hub started after boot. It does not fight peer
islands. A board's own AP sits on **192.168.99.1** (not the ESP default
192.168.4.1) so its STA can pull a clean 192.168.4.x lease from a hub — else it
associates but can't route (two interfaces, one subnet) and wrongly islands.
`discover_hub` retries the scan 3× (a single active scan can miss a present AP).
All APs **open** by default. mDNS: a board is **`rover.local`**, a hub is
`hub.local` (a board never claims `hub.local` — it would collide with the Pi).
The always-on AP keeps `http://rover.local/` reachable for the #17 config panel
("set your home Wi-Fi" = the home switch); cost is per-board beacons in a
classroom (measure-then-mitigate; `hub#3` closed 2026-07-10 — the per-board-AP
topology question dissolved into always-APSTA).

> **Status (2026-07-09):** always-APSTA board built and **hardware-validated on
> the C3** — APSTA from boot, `rover.local`, island broker, drive, **no reboot**,
> stable past the pi-watch scan. Remaining: motor contention under drive load,
> the #17 Wi-Fi panel, tier-2 designate-as-hub. Tracker: robot#2. Owed: the Pi
> advertises `hub-pi-<suffix>` (`hub` repo).

### Rover role
One radio: Wi-Fi STA + esp-mqtt (BLE removed 2026-07-09 — see below).
Boot is a pure function of NVS; no stored state is ever a dead end. **Nothing
stored is fully operable**: no ssid → scan-join the strongest *open* `hub-*`
network (the classroom AP convention is the onboarding channel); no locator →
dial the DHCP gateway (on its own AP the hub is the gateway); no team → the
compile-time `unassigned` pool credential (professor-drivable only — no
student holds it) until the dashboard assigns one. Discovery results
are never persisted — and **a hub in range wins over the stored network** (the
classroom IS the venue; fixed 2026-07-10 — stored-first made a home-configured
board reboot-loop off `hub_watch` instead of ever joining a classroom hub). Only
when no `hub-*` is found does the stored ssid join, and a stored locator rides
only its own stored network (half-stale config isn't trusted by halves).

```
boot ──► [dispatcher: role_pref] ──► rover role ──► Wi-Fi STA: discover open hub-*, else stored ssid
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
  admits it and teams can't cross. Compile-time `unassigned` (`-DMQTT_USER`) is
  the fallback — a first-class pool identity (2026-07-10; was demo `team1`,
  which let fresh boards collide on a real team's card and obey its
  student-known drive credential); the real team is assigned post-join from
  the dashboard (`robots/<id>/cmd/config` → NVS).
- **`rover-XXXX`** (last 2 MAC bytes via `rover_format_robot_id`) is a `board`
  field in the sys payload — hardware is metadata, never the topic id.
- **`role_pref`** (NVS key `role`, one byte; `rover_config_load/set_role_pref`,
  enum `AUTO`=0/`HUB`=1/`ROVER`=2) selects the boot role (§ Boot flow). Unset or
  unrecognized → `AUTO`, so stale/garbage NVS never wedges an unknown role.

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

## esp-mqtt API notes (ESP-IDF 5.5)
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
  blob, a custom-wired chassis), `role` (boot role, § Identity). All optional —
  absent falls back to the compile-time / AUTO default.

## Status
**v6 (unified image) — builds green 2026-07-09; hub role not yet hardware-run.**
One firmware now carries both roles behind the `role_pref` dispatcher (§ Boot
flow). Build-verified on both arches: `esp32dev`/xtensa 48% flash, `esp32c3`/riscv
51% (~1.5 MB of the 3 MB partition) — the two MQTT stacks (embedded broker +
esp-mqtt client) and the 408 KB embedded dashboard coexist in one image.

- **rover role** — hardware-validated at **v5** (2026-07-09) on rover-a044
  (ESP32-D0WD) + rover-b79c (ESP32-C3): boot → join open `hub-*` → connect as the
  NVS team → publish + drive; motor init, LED-on-connect, NVS persistence across
  reflash confirmed. Unchanged by the fold (same code, now `rover_role.c`).
- **hub role** — folded in from the former `hub/esp32` project (feasibility-
  validated there on hardware; that standalone project was **removed** from the
  `hub` monorepo once its source landed here). Forced via `role_pref = HUB`
  (tier 2), or entered via a tier-3 self-hub. Boot-run on hardware 2026-07-09.
- BLE gone since v5 (#11), freeing ~175 KB flash + ~44 KB heap.

**Direction changed 2026-07-09: election → role tiers** (§ Boot flow;
`DESIGN-unified.md` § Direction change; **robot#2**). The distributed election
shipped (`5b5b49d`) then was superseded by the tier model (rover / hub-only /
auto-self-hub) — **islands, not attraction**: a self-hub board raises an open
`rover-<id>` AP (not `hub-*`), so nothing joins it, and a shared broker is opt-in
via a Pi or a tier-2 `hub-*`. A tier-3 board yields only to `hub-pi-*`. **Built &
HW-validated** (`42a73b4`): election deleted, tier-3 combined HUB+ROVER run-path
(`rover_client_run` against the loopback broker), self-hub survives reboot
(`RTC_NOINIT`), open APs. **Remaining:** motor contention under drive load
(`hub#2`); the #17 **Wi-Fi config panel**; **tier 2** designate-as-hub + first
classroom run. **Owed:** the Pi advertises `hub-pi-<suffix>` (`hub` repo).
**Resolved (hub#3, closed 2026-07-10):** the per-board-AP-vs-shared-hub dichotomy
dissolved — always-APSTA runs both, split by hub presence; the beacon/client-cap
cost stays measure-then-mitigate.

History (git): v2 zenoh firmware (three-rover fleet, BLE provisioning); v3/v4 the
MQTT port + motor drive; v5 BLE removed; v6 the unified rover+hub image.
