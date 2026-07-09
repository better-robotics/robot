# Unified ESP32 firmware — one image, role at boot, captive-portal config

**Status:** in progress (2026-07-09). This design now lives with the firmware in
`better-robotics/robot` — the two ESP32 codebases have been collapsed into this
repo (the standalone `hub/esp32` project was removed once its source folded in).
Done: BLE removed (#11); **step 1** feasibility (combined image links, ~51% of
3 MB); **step 2** the boot dispatcher; **step 3** the **always-APSTA board**
(`board_run` in `hub_role.c`) — one path, no mode-switch reboot, home/classroom
at runtime — hardware-validated on the C3 (`rover.local`, island broker, no
reboot). Next: the per-board **Wi-Fi config panel** (#17) that fills the
`rover.local` "set your home Wi-Fi" seam, and motor contention under drive load.

## Direction change — election → role tiers → always-APSTA (2026-07-09)

The distributed **election** (grace window + MAC jitter + lowest-MAC tiebreak +
abdication) was built and committed (`5b5b49d`), then **replaced** by role tiers,
which were then **simplified again** into the always-APSTA model below. The
election existed to auto-pick *one shared ESP hub* among several student boards —
imitating the Pi in software, on worse hardware, at the design's highest
complexity. Both later passes drop the distributed part entirely.

- **One always-APSTA board path, `role_pref` selects a rare override:**
  - **auto** *(default)* — the normal board. Always **APSTA**: raises its own
    **open `rover-<id>` AP** *and* an STA uplink from boot, and **never switches
    radio mode**. Finds a `hub-*` → drives off that shared broker (classroom);
    finds none → runs a **local broker** and **drives itself** (home/island).
  - **rover** — the same board path, but **pinned not to self-broker**: no hub in
    range → it keeps looking (never becomes an island).
  - **hub** *(tier 2 — professor / designated ESP hub)* — a **dedicated** hub:
    `hub-*` AP + broker + dashboard + NAT, **does not drive**. An ESP32 optionally
    *replaces the Pi* this way.
- **No mode-switch reboot.** Because the board is APSTA from line one, home↔
  classroom is **runtime state**, not a boot role — the drive client just re-points
  its broker URI (`mqtt://<pi>:1883` vs `mqtt://127.0.0.1:1883`), no reboot. The
  old self-hub **claim-by-reboot** (`role_boot_as_hub` / `role_pending_hub_boot` /
  the `RTC_NOINIT` flag) is **deleted** — it existed only to avoid a live STA→
  APSTA switch, which no longer happens. (That machinery was also the source of
  two hardware bugs: the `RTC_DATA` wipe reboot-loop, 2026-07-09.)
- **The board's AP is always reachable.** Even in a classroom the board keeps its
  `rover-<id>` AP up, so its config surface (`http://rover.local/`) is always
  reachable — the seam the `#17` Wi-Fi panel fills ("go to rover.local, set your
  home Wi-Fi" = the home switch). Cost: every board beacons its own AP → classroom
  co-channel congestion (this is `hub#3`, measured-not-assumed; if it bites, drop
  the beacon when cleanly joined to a Pi).
- **Islands by default, not attraction.** A self-broker board's AP is named after
  its **rover-id** (`rover-<id>`), *not* `hub-*` — so no other rover's `hub-*`
  discovery ever latches onto it. Each is its own **island**: one board, one AP,
  one broker, one dashboard, one rover; the student joins *their* `rover-<id>`.
  A **shared** broker (central control — one dashboard, per-team ACL on the Pi) is
  opt-in: an explicit hub — a **Pi**, or a board **pinned to `role_pref=HUB`**.
- **Hub-preference.** An island board keeps watching for any **`hub-*`** and steps
  down to it via a **clean restart** (not a mode switch, no RTC flag — `board_run`
  re-runs, discovers the hub, joins it). This is safe against peer islands *because*
  islands raise `rover-<id>`, not `hub-*` — so a `hub-*` beacon can only be a real
  designated hub (a Pi `hub-pi-*` or a tier-2 professor hub), never another home
  board. It also self-heals a boot scan that missed the hub, and lets a board join
  a hub switched on *after* it booted. (Owed: the Pi advertises `hub-pi-<suffix>`.)
- **The board's own AP sits on `192.168.99.1`, not the ESP default `192.168.4.1`.**
  A board keeps its AP up while its STA joins a hub — but the hub also leases from
  `192.168.4.0/24`, so a board on the default subnet associates yet never routes
  (two interfaces, one subnet → DHCP/routing breaks; it wrongly falls back to
  islanding). Relocating the board AP lets its STA pull a clean `192.168.4.x`
  lease. The dedicated hub keeps `.4.1` — it's the one boards join.
- **APs are open by default.** No password to join a hub or a rover island. An
  optional per-board WPA2 password can be set later (rides with the `#17` panel).
- **mDNS: `rover.local` for a board, `hub.local` for a hub.** A board never claims
  `hub.local` (it would collide with the Pi). On a board's own AP it is the sole
  responder, so `rover.local` is unambiguous; a shared LAN with several boards
  would collide there — the classroom uses the dashboard's per-id list, not
  `rover.local`.

**Why home mode is mandatory, not optional:** one kid, one board, no Pi, no
professor — the board must be its own hub *and* drive itself, or a single board
does nothing. So it's the home deployment context, first-class.

**Deferred to hub#3:** "*every* classroom board runs its own AP" (each student
joins their own rover, STA uplinks to the hub). Solves the ESP hub's ~8–10
client cap, but flips classroom control from **central** (one shared broker, ACL
team isolation, teacher drives any robot) to **local**, and adds RF co-channel
cost. Decision gated on measuring whether the client cap actually binds — see
hub#3 and § "Costs / risks".

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
boot → load NVS (role_pref, uplink wifi, team creds, motor pins)
     → dispatcher:
        ├─ role_pref = hub ──────► HUB (tier 2: hub-* AP + broker + NAT; no drive)
        └─ else ─────────────────► board_run: come up APSTA (own OPEN rover-<id>
                                   AP + STA), then LOOP (no reboot):
              ├─ stored uplink joins ──────► drive off its broker (locator) 
              ├─ open hub-* discovered ────► join it, drive off the gateway (classroom)
              └─ neither, and AUTO ────────► start local broker, drive 127.0.0.1 (island)
                                             │  (rover-pinned instead: rescan, no island)
                                             └─ watch for any hub-* → clean restart
                                                → re-run board_run → join the hub
              (on a dead drive session: re-evaluate the same loop — no reboot)
```

The board is **APSTA from line one and never switches radio mode**, so there is
no claim-by-reboot: home↔classroom is a runtime re-point of the broker URI. Only
a `role_pref=HUB` board takes the separate dedicated-hub path (`hub-*`, no drive).

## Islands & Pi-preference (always-APSTA model)

No distributed election (see § Direction change). The topology is explicit, not
emergent, and decided at runtime — the board never reboots to change it:

- **Islands by default** — a self-broker board raises an **open `rover-<id>` AP**,
  *not* a `hub-*`. Rovers only ever join `hub-*` (discovery), so nothing joins a
  self-broker board: each is a self-contained island (own AP + broker + dashboard
  + rover). Two home boards side by side simply coexist as two islands — no
  election, no split-brain, no "self-heal" because there was never a shared thing
  to converge. A student drives *their* board by joining its `rover-<id>` AP.
- **Shared broker is opt-in** — central control (one dashboard for the room, the
  Pi's per-team ACL) requires an explicit hub: a **Pi**, or a board **pinned to
  tier-2** (`role_pref=HUB`) raising an open `hub-*`. Auto boards then discover
  that `hub-*` and drive off its broker instead of self-brokering. So the room is
  central when a hub is provided, per-board islands when it isn't — and a board
  **degrades gracefully without a reboot**: lose the hub mid-session and an AUTO
  board's next loop pass islands itself; the hub returns and Pi-watch rejoins it.
- **Hub-preference** — an island board keeps scanning and yields to any **`hub-*`**
  via a **clean restart** (not a mode switch, no RTC flag) — `board_run` re-runs,
  discovers the hub, joins it. Safe against peer islands because they advertise
  `rover-<id>`, never `hub-*`; so this covers the slow-Pi race (Pi appears ~30–60 s
  after an ESP's ~1 s boot), a boot scan that *missed* a present hub, and a hub
  switched on after boot — all with one rule. **Owed (`hub` repo):** the Pi
  advertises `hub-pi-<suffix>` so it's distinguishable as *the* preferred hub (for
  a future rule that prefers a Pi over a peer tier-2 ESP hub).

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

- **Classroom co-channel congestion** is the live make-or-break of always-APSTA:
  every board beacons its own `rover-<id>` AP, forced onto the Pi's channel
  (single radio). Measure whether the beacons/client-cap actually bite a real
  room; if they do, drop a board's AP beacon when it's cleanly joined to a Pi
  (`hub#3`). This replaced the old election-correctness risk (election deleted).
- **Binary size:** broker + AP+NAT + httpd + WS bridge *plus* the drive client in
  one image — ~51% of the 3 MB factory partition (built on both xtensa + riscv,
  ESP32-CAM included). RAM is fine (~150 KB free on the C3 with the full stack).
- **ESP-hub has no per-topic ACL** (connect-auth only) — an all-ESP fleet loses
  the Pi's per-team isolation (convention-only). Accepted for the no-Pi
  fallback; the Pi keeps full ACL.
- **Motor contention:** an **island** board drives *while* running its own broker+
  AP (broker/AP vs real-time motors on one radio+CPU — `hub#2`, still unmeasured
  under load). A dedicated tier-2 `role_pref=HUB` board deliberately does **not**
  drive, sidestepping it. This is the last untested tier-3 aspect.
- **Open AP:** anyone nearby can join a board's open AP (and, once #17 lands,
  configure it). Acceptable for a classroom; an optional per-board WPA2 password
  rides with #17 if not.

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
