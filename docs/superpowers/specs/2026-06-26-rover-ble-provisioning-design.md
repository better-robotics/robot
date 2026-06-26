# Rover BLE provisioning + hub discovery — design

**Date:** 2026-06-26
**Status:** approved (design); pending implementation plan
**Repos touched:** `better-robotics/rover` (firmware, bulk), `better-robotics/provision` (web, small)

## Problem

The rover firmware hardcodes everything network-specific in `src/main.c`:

```c
#define WIFI_SSID   "DukeVisitor"
#define WIFI_PASS   ""
#define HUB_LOCATOR "tcp/172.28.143.53:7447"
#define ROBOT_ID    "esp32_01"
```

Every network change or hub-IP change means editing source and reflashing. The
goal: provision Wi-Fi **and** the hub locator over BLE, with no reflash — the same
cable-free onboarding the hub already has.

### Why not Zenoh multicast scouting

The obvious "rover discovers the hub automatically" answer is Zenoh scouting
(`z_scout`, UDP multicast `224.0.0.224:7446`). It is rejected because the target
networks block it. `hubd.rs`'s own header says the hub is designed for
"multicast-blocked Wi-Fi" — peers point at a bootstrap endpoint and gossip does the
rest. `DukeVisitor` is a university guest network (multicast filtering + client
isolation typical). So the rover must be *told* the one bootstrap locator it cannot
discover. We provision it over BLE alongside Wi-Fi.

## Approach

Mode-switched firmware on **NimBLE**: BLE provisioning mode when unconfigured,
Wi-Fi operating mode once provisioned. The two radios never run at once — this
sidesteps Wi-Fi/BLE coexistence on the classic ESP32-D0WD's single shared 2.4 GHz
radio, and saves the coexistence RAM. Rejected alternatives: concurrent BLE+Wi-Fi
(coexistence risk, RAM/flash cost, only buys reboot-free re-provisioning that a
one-time setup doesn't need); Bluedroid (heavier flash, no benefit over NimBLE).

Wi-Fi is provisioned with **stock Improv** (unchanged protocol, same hosted-style
client as the hub). The hub locator — which Improv has no field for — rides a
single **custom GATT characteristic**. That characteristic is the only new protocol
element in the system.

## Architecture

```
┌─ better-robotics/provision (web) ─────────┐      ┌─ rover (ESP-IDF firmware) ──────────┐
│  rover.html:                              │ BLE  │  Provisioning mode (no NVS config): │
│   1. stock Improv launch-button ──────────┼─────▶│   • NimBLE GATT server advertising  │
│      → Wi-Fi SSID + password (Improv)     │      │     Improv (Wi-Fi) + hubcfg service │
│   2. "Set hub address" control ───────────┼─────▶│   • persists creds + locator to NVS │
│      → writes hubcfg locator char         │      │                                     │
│      (pre-filled with the hub IP)         │      │  Operating mode (NVS has config):   │
└───────────────────────────────────────────┘      │   • Wi-Fi STA → zenoh client (BLE   │
                                                    │     off) → publish telemetry        │
   no hub firmware change                           └─────────────────────────────────────┘
```

## The interface contract (`hubcfg` GATT service)

The single new protocol element. Both ends agree on this; everything else is stock
Improv.

- **Service UUID:** a fresh 128-bit UUID, distinct from Improv's
  `00467768-…`. (Final value chosen at implementation; recorded here and in both
  codebases as the canonical constant.)
- **Characteristic — `locator`:** writable (write-with-response), and readable for
  verification. Value: a UTF-8 string `tcp/<host>:<port>`, e.g.
  `tcp/192.168.1.42:7447`.
- **Validation (rover side, before persisting):** must start `tcp/`; host is a
  dotted IPv4 or hostname (no spaces/control chars); port is 1–65535; total length
  bounded (≤ 64 bytes). Reject otherwise — same defense-in-depth posture as the
  hub's Improv credential validation (`hub::improv::parse_wifi`).

The locator is written as a plain config string, not an Improv RPC frame — it is
deliberately outside Improv so Improv stays stock.

## Firmware state machine

```
boot → nvs_read(config)
  ├─ config absent  → PROVISIONING: wifi off, NimBLE on,
  │                    advertise rover-XXXX (Improv + hubcfg).
  │                    on each write: persist to NVS.
  │                    when BOTH wifi creds AND locator present → esp_restart()
  │
  └─ config present → OPERATING: BLE off, wifi STA (from NVS),
                       zenoh client → HUB_LOCATOR (from NVS), publish telemetry.
                       if wifi-join OR z_open fails N consecutive times
                       → clear a "use BLE" flag and esp_restart() into PROVISIONING.
```

- **Re-provisioning trigger:** N consecutive operating-mode failures (Wi-Fi
  association or `z_open`) drops the rover back into provisioning mode, so a moved
  hub or changed network is recoverable cable-free. N and the backoff are tunables
  set in the plan (start: N=5, ~3 s apart, matching the current restart loop feel).
- **NVS schema:** namespace `rover`, keys `ssid`, `pass`, `locator`. A single
  "configured" predicate = ssid present AND locator present.

## Identity (removes two hardcodes)

Derive `ROBOT_ID` from the ESP32 base MAC as `rover-XXXX` (last 2 bytes, hex),
matching the BLE advertised name — the same pattern the hub uses (`hub-XXXX` from
chip serial). Telemetry key becomes `robots/rover-XXXX/sys`. Unique per board, no
collisions, nothing hardcoded. `WIFI_SSID`/`WIFI_PASS`/`HUB_LOCATOR` `#define`s are
removed; values come from NVS.

## Provision page (`better-robotics/provision/rover.html`)

Sibling to the hub page. Two controls, two BLE chooser prompts (acceptable for
one-time setup):

1. **Set Wi-Fi** — the stock `<improv-wifi-launch-button>` (vendored SDK already in
   the repo), targeting `rover-XXXX`. Unchanged Improv flow.
2. **Set hub address** — a small Web-Bluetooth control: connect to the rover, write
   the `hubcfg` `locator` characteristic, read it back to confirm. The input is
   pre-filled with the hub IP (operator pastes the `tcp/<ip>:7447` line the hub's
   login banner already prints).

The hub page (`index.html`) is untouched.

## Out of scope (YAGNI)

- Concurrent BLE + Wi-Fi (mode-switch instead).
- Any Improv protocol extension (locator is a separate characteristic).
- Hub firmware changes (already provisioned + status-banner'd).
- OTA updates, a rover-hosted web UI, encrypted/authenticated BLE pairing
  (classroom threat model = physical proximity, same as the hub's open Improv).

## Testing

- **Host-testable:** locator validation and the NVS "configured" predicate factored
  so their logic can be exercised off-device where practical (ESP-IDF host build or
  extracted pure functions).
- **On-hardware (the real proof, per repo convention "intended vs verified"):**
  1. Flash unconfigured → boots into BLE provisioning mode, advertises `rover-XXXX`.
  2. From `rover.html`: Improv sets Wi-Fi; "Set hub address" writes the locator.
  3. Rover reboots → joins Wi-Fi → opens zenoh session → publishes
     `robots/rover-XXXX/sys`, seen live on the hub.
  4. Power-cycle → rejoins from NVS with no BLE (persistence).
  5. Point the locator at a dead address → after N failures, falls back to BLE
     provisioning mode (re-provisioning).

## Primary risk

Flash/RAM budget: NimBLE + Wi-Fi + zenoh-pico on the 4 MB classic ESP32 (already on
the single-app-large partition). Mode-switching (never both radios live) is what
makes this likely to fit; **firmware size is verified early in implementation**, not
at the end. If it doesn't fit, fallback options (in order): trim NimBLE features,
drop the readable-back verification on `locator`, separate provisioning firmware.
