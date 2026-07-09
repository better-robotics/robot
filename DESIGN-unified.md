# Unified ESP32 firmware — one image, role at boot, captive-portal config

**Status:** in progress (2026-07-09). This design now lives with the firmware in
`better-robotics/robot` — the two ESP32 codebases have been collapsed into this
repo (the standalone `hub/esp32` project was removed once its source folded in).
Done: BLE removed (#11); **step 1** feasibility (combined image links, ~48% of
3 MB); **step 2** the boot-role dispatcher (`role_pref` NVS → `rover_role_run` /
`hub_role_run`); **step 3** the election protocol (below) — **code-complete,
build-green both arches (2026-07-09); NOT yet hardware-convergence-tested** (the
make-or-break two-board / slow-Pi test, § Costs/risks). Next: **step 4** the
config page (uplink `setup-*` AP + LED-per-role + force-rover button). See
`CLAUDE.md` › Boot flow.

**Step 3 as built:** AUTO rover, no `hub-*` in range → `run_election`
(`rover_role.c`): MAC-jittered grace window (`ELECTION_GRACE_MS` 60 s +
≤20 s/MAC), rescanning; any `hub-*` appears → yield/rover; window elapses →
claim via a transient RTC flag (`role_boot_as_hub`, `main.c`) + reboot into the
hub role (avoids a STA→APSTA re-init in one boot; a power cycle re-elects).
Abdication (`hub_role.c`, elected hubs only, ~3-min window): step down (reboot →
re-elect → rover) on a `hub-pi-*` marker or a lower-BSSID `hub-*`. **Companion
change still owed (mitigation c): the Pi must advertise `hub-pi-<suffix>`** so an
already-claimed ESP always yields to it — until then Pi-preference rests on the
grace window (b) alone.

Collapse the two ESP32 firmwares — the on-chip hub and the rover — into **one
image** whose role is decided at boot, and replace **BLE onboarding** with an
on-device **captive-portal config page**. The Pi stays the preferred hub; ESP
self-election is a *fallback*, not the default.

## Why

- **Kill the BLE stack.** NimBLE + Improv + the `hubcfg` GATT characteristic +
  the `provision/rover.html` Web Bluetooth page — all gone, replaced by a Wi-Fi
  config page every board can already serve (the hub serves one today).
- **One image to flash** everywhere; role is runtime, not build-time.
- **No-Pi demos** become possible: a lone ESP board can bootstrap a hub.
- Rovers are *already* zero-touch (scan open `hub-*`, derive broker from the
  gateway); BLE was only ever the override path — so we're deleting the
  rarely-used half, not a load-bearing one.

## Boot state machine

```
boot → load NVS config (role_pref, uplink wifi, team creds, motor pins)
     → scan for open hub-* APs
        │
        ├─ hub-* found, role_pref ≠ hub ─────────────► ROVER MODE   (today's path, unchanged)
        │      join → gateway:1883 → MQTT client → telemetry + drive
        │
        ├─ no hub-*, have config, role_pref ∈ {auto,hub} ─► HUB ELECTION
        │      won  → HUB MODE: AP hub-<mac> + STA(uplink) + NAPT + mosquitto
        │             + WS bridge + serve dashboard + captive config page
        │      lost → ROVER MODE, join the winner
        │
        └─ no hub-*, no usable config ───────────────► CONFIG MODE
               start AP setup-<mac> + captive portal; user picks a path; save; reboot
```

Only **one role runs per boot** — so the binary is bigger but runtime RAM stays
per-role (hub OR rover, never both live at once).

## Hub election — the part that bit us

The split-brain we just debugged (two hubs, fleet divided) **is** naive
self-election's failure mode. Election must converge to exactly one hub:

1. **Re-scan** right before claiming — a hub may have appeared during boot.
2. **Randomized backoff** (0–`T` s, spread by MAC) while continuing to scan;
   any `hub-*` that appears → **yield**, become a rover.
3. Backoff expires with no hub → **claim**: raise the AP + broker.
4. **Lowest-MAC-wins tiebreak:** after claiming, keep scanning; if a `hub-*`
   with a *lower* MAC is seen, **step down** and join it. Guarantees eventual
   convergence even if two claim in the same instant.

**Prefer the Pi — and mind the slow-Pi race.** The Pi's AP is also `hub-*`, so
once it's up, ESP boards find it → rover mode → never elect. But the Pi boots in
**30–60 s** while ESP32s boot in **~1 s**: rovers powered *with* the Pi (one
power strip — the classroom norm) will elect an ESP hub *before* the Pi's AP
exists, then must **abdicate** when it appears, disconnecting the class
mid-lesson. Mitigations: (a) the elected ESP hub keeps scanning and steps down
to the Pi (abdication is unavoidable if you allow early election); (b) better —
**gate election behind a longer grace period** (e.g. 60–90 s of scanning) so a
booting Pi wins before any ESP claims; (c) hard guarantee — the Pi advertises a
reserved marker SSID (`hub-pi-*`) that ESP boards *always* yield to. Given the
correlated-power-on reality, lean on (b)+(c), not "always-up-first."

## Onboarding: post-join config, not a captive portal (cold-pass revision)

The first draft replaced BLE with a **captive portal**. A cold pass (2026-07-09)
argued that's the wrong replacement for classrooms — MDM-managed
Chromebooks/iPads often can't join arbitrary open APs, OSes auto-drop
no-internet networks, and it forces every student off the school network. The
stronger move it surfaced: **rovers already auto-join the open `hub-*` AP, so
there is nothing to onboard *before* join.** Move per-rover config to *after*
join:

- Turn a rover on → it joins `hub-*` (zero config) → announces itself over MQTT.
- The **hub dashboard** lists unassigned rovers; the teacher names one, assigns
  its **team**, and (for a student-wired chassis) sets its **motor/encoder
  pins** — pushed back over MQTT (`robots/<id>/cmd/config`), saved to NVS.
- Result: **BLE *and* the captive portal are both cut.** Onboarding a rover is:
  turn it on.

The **only** config that can't be post-join is the **hub's own uplink Wi-Fi**
(the STA leg needs creds before it has internet). For the Pi that's the Pi's own
setup; for an ESP32 *fallback* hub, keep a **minimal** SoftAP config page for
uplink creds only — the rare case, not the rover path. So the captive portal
shrinks from "the onboarding system" to "an ESP-hub uplink form."

## Role must be loud and overridable

A rover that silently self-elected hub just… doesn't drive, with no signal why.
So role is **visible and forceable**:
- **LED per role** — distinct colors/patterns for rover · hub · config.
- **Hold BOOT at power-on = force rover** (teacher override for a mis-elected
  board; the ESP32-CAM uses the `reprovision` topic instead).
- Disjoint SSID namespaces: hubs are `hub-*`, config APs are `setup-*`; role
  logic matches `hub-` strictly so an unconfigured board never enrolls into a
  classmate's `setup-*` screen.

## NVS config schema

| key | role | today |
|-----|------|-------|
| `role_pref` | auto / hub / rover | new |
| `uplink_ssid` / `uplink_pass` | hub STA uplink | compile-time `wifi_creds.h` |
| `team_user` / `team_pass` | rover MQTT identity | compile-time `-DMQTT_USER/PASS` |
| `motor_*` (ena,in1..4,enb,enc×2) | rover wiring | compile-time `#define` |
| `locator` | explicit broker override | exists (`rover_config`) |

Compile-time defaults stay as fallbacks; NVS overrides them.

## What's retired

- `improv.c`, `provisioning.c` (BLE), the `hubcfg` characteristic, NimBLE deps.
- `provision/rover.html` (Web Bluetooth) → the on-device captive portal.
- Separate `esp32/` hub image + `robot/` rover image → **one image** (the rover
  repo folds in, or both build from shared `main/`).

## Costs / risks (honest)

- **Election correctness** is a real distributed-systems task — must be tested
  with two boards + no hub, confirming exactly one yields. This is the make-or-
  break.
- **Binary size:** hub role (broker + AP+NAT + httpd + WS bridge) *plus* rover
  role in one image — validate it fits the partition, ESP32-CAM being the tight
  case. (RAM is fine: per-role at runtime.)
- **ESP-hub has no per-topic ACL** (connect-auth only) — an all-ESP fleet loses
  the Pi's per-team isolation (convention-only). Accepted for the no-Pi
  fallback; the Pi keeps full ACL.
- **Motor contention:** a HUB-mode board should **not** drive (broker/AP vs
  real-time motors share one radio+CPU — `hub#2`). Only ROVER mode drives.
- **Open config AP:** anyone nearby can configure a board in CONFIG mode.
  Acceptable for a classroom; add a short PIN if not.

## Migration

1. **Post-join config first** — rover announces on join; add a
   `robots/<id>/cmd/config` topic + NVS write for name/team/pins, and a dashboard
   panel to assign them. Prove it end-to-end while BLE still exists (de-risk).
   This is the rewrite of #6 (away from BLE, away from a captive portal).
2. **Delete the BLE stack** once post-join config covers the rover cases
   (`improv.c`, `provisioning.c`, `hubcfg`, NimBLE, `provision/rover.html`).
3. **Fold the hub role into the rover image** (one firmware) with **role
   election** — gated grace period + jitter + lowest-MAC tiebreak + abdication;
   LED-per-role + force-rover button. Test two-board and slow-Pi convergence.
4. Keep a **minimal ESP-hub uplink config AP** (`setup-*`) — the only surviving
   captive-portal remnant, for the no-Pi fallback only.
5. Retire the separate `robot` repo (or make it the shared `main/`).

## Open questions

- Pi-preference: rely on always-up-first, or a reserved `hub-pi-*` marker?
- Hub board driving motors: forbid (recommended) or allow degraded?
- Config-AP security: open, or a PIN?
- Does the rover repo fold into `esp32/`, or does a shared `main/` feed both?
