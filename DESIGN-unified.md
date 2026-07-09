# Unified ESP32 firmware — one image, role at boot, captive-portal config

**Status:** in progress (2026-07-09). This design now lives with the firmware in
`better-robotics/robot` — the two ESP32 codebases have been collapsed into this
repo (the standalone `hub/esp32` project was removed once its source folded in).
Done: BLE removed (#11); **step 1** feasibility (combined image links, ~48% of
3 MB); **step 2** the boot-role dispatcher (`role_pref` NVS → `rover_role_run` /
`hub_role_run`). Next: **tier 3 — home mode** (self-hub+rover, below), then the
per-board **Wi-Fi config panel** (#17) it needs.

## Direction change — election → role tiers (2026-07-09)

The distributed **election** (grace window + MAC jitter + lowest-MAC tiebreak +
abdication) was built and committed (`5b5b49d`), then **replaced** by a simpler
model after a design pass. The election existed to auto-pick *one shared ESP hub*
among several student boards — but that is imitating the Pi in software, on
worse hardware, at the design's highest complexity (the split-brain "make-or-
break"). The replacement drops the distributed part entirely:

- **Three role tiers, one firmware, `role_pref` selects:**
  - **rover** — STA client, joins a hub, drives. (needs a hub present)
  - **hub** *(tier 2 — professor / designated ESP hub)* — AP + broker + dashboard
    + NAT; **does not drive**. An ESP32 optionally *replaces the Pi* this way.
  - **auto** *(default)* — finds a `hub-*` → become its **rover**; finds none →
    **self-hub+rover** *(tier 3 = home mode)*: raise own AP + local broker +
    dashboard, and **drive itself**.
- **Convergence by *attraction*, not election.** A self-hub board raises a
  `hub-*`; boards that boot later discover it and join as rovers — a fleet forms
  with the first-booted as the hub, no protocol. The only unhandled case is
  *simultaneous* boot (two boards self-hub before seeing each other → transient
  islands); it self-heals on reboot and is acceptable (the firmware's existing
  "reboot to converge" DNA).
- **Pi-preference, minimal.** A self-hub board keeps watching *only* for a Pi
  (`hub-pi-*`) and steps down to it (the slow-Pi race). It does **not** fight
  peer ESP hubs — peer islands are fine. This is the one deterministic yield
  kept from the election's abdication; grace/jitter/lowest-MAC are deleted.
  (Owed with it: the Pi advertises `hub-pi-<suffix>`, a `hub`-repo change.)

**Why tier 3 is mandatory, not optional:** *home mode* = one kid, one board, no
Pi, no professor — the board must be its own hub *and* drive itself, or a single
board does nothing. So tier 3 is the home deployment context, first-class.

**Kept from the election:** the reboot-based role switch — a transient RTC flag
(`role_boot_as_hub`/`role_pending_hub_boot`, `main.c`) so a role change is a
clean reboot into a fresh radio init, never a STA→APSTA switch mid-boot.

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
        ├─ elected-hub RTC flag ─► HUB+ROVER (tier 3, this boot only)
        ├─ role_pref = hub ──────► HUB (tier 2: AP+broker+dashboard+NAT; no drive)
        ├─ role_pref = rover ────► ROVER (join a hub-*, drive)
        └─ role_pref = auto ─────► scan for open hub-*
              ├─ found ──────────► ROVER: join it, drive           (attraction)
              └─ none ───────────► self-hub: set RTC flag, reboot → HUB+ROVER (tier 3)
```

HUB+ROVER (tier 3) keeps watching for a Pi (`hub-pi-*`) and steps down to it.
Only **one role runs per boot** — the binary is bigger but runtime RAM stays
per-role.

## Convergence & Pi-preference (post-election model)

No distributed election (see § Direction change). Convergence is *emergent*:

- **Attraction** — rovers already join any `hub-*` they find, so the first board
  to self-hub becomes the de-facto hub and later boards join it. A fleet forms
  with no protocol; only *simultaneous* self-hub → transient islands, self-healed
  by a reboot. Acceptable — not a split-brain we must prevent, a rare case we let
  the existing reboot-loop resolve.
- **Pi-preference** — a self-hub (tier 3) board keeps scanning and yields **only**
  to a Pi (`hub-pi-*`), whatever its MAC. It does *not* step down for peer ESP
  hubs. This is the single deterministic rule kept from the old abdication; the
  slow-Pi race (Pi's AP appears ~30–60 s after an ESP's ~1 s boot) is handled by
  the tier-3 board seeing `hub-pi-*` and rebooting into rover mode.
  **Owed (`hub` repo):** the Pi advertises `hub-pi-<suffix>` — until then an
  already-self-hubbed ESP has no marker to yield to.

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
