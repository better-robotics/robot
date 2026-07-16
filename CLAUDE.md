# robot — project context

ESP32 firmware for the classroom Robotics Hub
([`better-robotics/hub`](https://github.com/better-robotics/hub)). **One unified
image, APSTA at boot** (self-election collapsed into an AP+STA board 2026-07-09;
the AP drops while joined to a hub 2026-07-15 — see below):

- **`board_run`** (`hub_role.c`, the norm): every board comes up **APSTA** — its
  own open `rover-<id>` AP *and* an STA uplink — and **drops the AP for as long
  as it is joined to a hub** (one network in the room: the hub's). It's an
  `esp-mqtt` client that drives an L298N from `robots/<id>/pwm` and takes its name
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
zenoh-pico has no usrpwd, and esp-mqtt authenticates with username/password
natively. That capability turned out to gate only one identity in the end:
the classroom's real boundary is its own Wi-Fi, not a login (confirmed
2026-07-13) — every robot and browser gets open read+write, and `instructor`
is the sole credential, scoped to `fleet/estop` alone (CONTRACT.md § Discovery
& isolation). See git history for the zenoh-era firmware.

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
  `rover-l298n` (extends esp32dev). Each passes `-DROVER_NAME='"unassigned"'`
  (the pool name, no credential attached) as a *fallback* only — a rover's
  name is assigned post-join over MQTT (`robots/<id>/cmd/config` → NVS) from
  the dashboard's "Assign a rover" panel, so boards flash identically and get
  named at the hub.
- Platform pinned **`espressif32@6.13.0`** (ships **IDF 5.5.3**, not 5.1 as an
  earlier note claimed) for reproducible builds — the old `<7.x` constraint was
  zenoh-pico's; the pin is now just stability (and 5.5.x is what the mosquitto
  component resolves against).
- **CI** (`.github/workflows/firmware.yml`, verified green 2026-07-14): on push,
  all three board images build from a clean clone (`tools/sync-ide.sh` +
  `wifi_creds.example.h` stand-in cover the two gitignored seams), `pio test -e
  native` runs the Unity suite, and `pio check` (cppcheck, config in
  platformio.ini) gates at medium+. Flashable `.bin` trios upload as 7-day
  artifacts — the upload runs *before* check, whose cold-idedata configure
  wipes the build dir in CI.
- **Dashboard embed (hub role):** PlatformIO's SCons build does **not** wire an
  objcopy/`.S` embed into the link — `EMBED_TXTFILES`, `target_add_binary_data`,
  and `board_build.embed_txtfiles` all fail (missing `.S`, or generated-but-not-linked).
  So `tools/embed_dashboard.py` (a **pre-build** `extra_scripts` hook; uses
  `$PROJECT_DIR`, not `__file__`, which SCons doesn't define) generates
  `src/dashboard_html.c` — the dashboard as a plain byte array — from
  `web/dashboard.html`, compiled as an ordinary source. `web/dashboard.html` is a
  VENDORED copy (canonical in the `hub` monorepo); `tools/sync-dashboard.sh`
  resyncs it and `--check` gates drift. `src/dashboard_html.c` is gitignored.
- **IDE embed (hub role):** same mechanism, second bundle — `tools/embed_ide.py`
  generates `src/ide_bundle.c` (one gzip blob per file + a path/type table,
  `src/ide_bundle.h`) from `web/ide/`, and `ws_mqtt_bridge.c` serves it at
  `/ide/?*` with `Content-Encoding: gzip` (browser inflates; the chip never
  does). `web/ide/` is VENDORED from `better-robotics/ide`'s **release asset**
  `ide-esp32-dist.tar.gz` (Blockly + textarea editor, no Monaco — the bundle
  only exists built, it carries ide's gitignored vendor tree): `tools/sync-ide.sh`
  fetches it, `--check` gates drift, and the tag pin lives IN the script —
  bumping it is a deliberate commit, never a build side effect. Both `:80`
  httpd configs use `httpd_uri_match_wildcard` for this route (exact URIs
  still match exactly).
- **Custom `partitions.csv`** (3 MB factory) — the unified image genuinely needs
  it now (~67–68% used with the embedded dashboard + block IDE); the
  "over-large, harmless" note from the lean rover-only era no longer applies.
- **`src/wifi_creds.h`** (gitignored) — the hub role's STA uplink credentials;
  copy `src/wifi_creds.example.h`. NEVER commit the real one.

## Boot flow

**Dispatcher first (`src/main.c`).** Shared init (NVS) runs once, then
`role_pref` (NVS key `role`, § Identity) picks the path and calls it (neither
returns):
```
app_main ─► role_pref == HUB → hub_role_run()   (tier 2: dedicated hub-* + broker + NAT; no drive)
            else             → board_run(self_broker_ok = role==AUTO)
                                 APSTA at boot: own OPEN rover-<id> AP + STA,
                                 loops: join hub-* → AP DOWN, drive its broker;
                                 else AUTO → local broker, drive 127.0.0.1 (island);
                                 else ROVER-pinned → rescan, never island.
                                 (AP down + no hub → restart, never a live switch)
```
**APSTA at boot; STA-only while a hub client** (2026-07-09, amended 2026-07-15;
§ Status & design history). The board is APSTA from line one, so home↔classroom is
a **runtime re-point** of the broker URI, not a boot role — the old
`role_boot_as_hub`/`RTC_NOINIT` claim-by-reboot is **deleted** (it only ever
existed to dodge a live STA→APSTA switch; it also caused two HW bugs — the
`RTC_DATA` wipe loop and the pi-watch stack panic). The one mode change that
remains is **APSTA→STA on a hub join** (`board_ap_down`), which is safe live —
subtractive, and the STA keeps its channel and association. **The way back is
never a live switch**: a board whose AP is down and whose hub is gone does a clean
`esp_restart` and comes up APSTA (the yield idiom, pointed the other way), so
STA→APSTA still never happens live. **Islands, not attraction:** a
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
All APs **open** by default.

**mDNS: two names, split by link** (2026-07-15). A board's **primary** name is its
unique **`rover-<id>.local`**, tracked on every netif — so it still answers on a
hub's LAN, full of peers. **`rover.local`** is a *delegated alias* pinned to the
board's own AP IP, so it exists only while the AP does: dropping the AP drops the
alias, with no separate rule. A hub is `hub.local` (a board never claims it — it
would collide with the Pi). **Unique-as-primary is load-bearing, not cosmetic:**
the primary is what gets mangled on collision, and RFC 6762 conflict resolution is
implemented (`mangle_name` → `-2`, `-3`), so N boards on one LAN don't fail —
`rover.local` silently resolves to **whichever booted first**, and the ordinals
(`rover-2`) land in the same namespace as MAC-suffix ids (`rover-3f2a`),
indistinguishable by shape. MAC suffixes don't collide, so a unique primary never
mints one. (Latent, needs a duplicate MAC to fire: `mangle_name` `strtol`s the
tail after the last `-`, so an all-decimal id — ~15% of boards, `(10/16)^4` —
mangles `rover-3210`→`rover-3211`, i.e. into another board's plausible id.)

The AP keeps `http://rover.local/` reachable for the #17 config panel ("set your
home Wi-Fi" = the home switch) **in the island case, which is the only case that
needs it** — a hub-joined board takes its name and pins over MQTT
(`cmd/config`), and its panel stays reachable at `rover-<id>.local` on the hub's
LAN. Per-board beacons in a classroom were the named cost; `hub#3`'s mitigation
("drop the beacon when cleanly joined to a hub") **shipped 2026-07-15** — see
`board_ap_down` for the three-part argument, of which the beacon cost is the
*weakest* third.

### Rover role
One radio: Wi-Fi STA + esp-mqtt (BLE removed 2026-07-09 — see below).
Boot is a pure function of NVS; no stored state is ever a dead end. **Nothing
stored is fully operable**: no ssid → scan-join the strongest *open* `hub-*`
network (the classroom AP convention is the onboarding channel); no locator →
dial the DHCP gateway (on its own AP the hub is the gateway); no name → the
compile-time `unassigned` pool name (drivable by anyone on the hub's Wi-Fi —
the open ACL doesn't single out unassigned boards, confirmed 2026-07-13)
until the dashboard assigns one. Discovery results
are never persisted — and **a hub in range wins over the stored network** (the
classroom IS the venue; fixed 2026-07-10 — stored-first made a home-configured
board reboot-loop off `hub_watch` instead of ever joining a classroom hub). A
stored **hub pin** narrows "a hub" to one exact SSID (`rover_hub_admits` —
discovery AND hub-watch), so a pinned board never joins a foreign `hub-*`. Only
when no `hub-*` is found does the stored ssid join, and a stored locator rides
only its own stored network (half-stale config isn't trusted by halves).

```
boot ──► [dispatcher: role_pref] ──► rover role ──► Wi-Fi STA: discover open hub-*, else stored ssid
         → mqtt connect(stored locator, or mqtt://<gateway>:1883) — no auth, as <name>
         → LED on; publish robots/<name>/sys every 2s
         → subscribe robots/<name>/{pwm, cmd/config, cmd/reprovision}
  │ can't join (30s) · no CONNACK in 10s (broker unreachable — no MQTT auth
  │ to reject) · ~20s dead session · button · reprovision message
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
- **Topic id == the name, and it's an address, not a credential** (confirmed
  2026-07-13). The rover connects with **no MQTT auth at all** and publishes
  under `robots/<name>/*` — every hub admits every name; the hub's own Wi-Fi
  is the real boundary, not a per-robot login. Compile-time `unassigned`
  (`-DROVER_NAME`) is the fallback — a first-class pool name (2026-07-10; was
  demo `team1`, which let fresh boards collide on a real robot's card); the
  real name is assigned post-join from the dashboard (`robots/<id>/cmd/config`
  → NVS, `{"name":"scout"}`, no password field). It may be driven by one
  student or a few sharing the board — the protocol has no notion of team
  size, only "whoever's on the hub's Wi-Fi drives it."
- **`rover-XXXX`** (last 2 MAC bytes via `rover_format_robot_id`) is a `board`
  field in the sys payload — hardware is metadata, never the topic id.
- **`role_pref`** (NVS key `role`, one byte; `rover_config_load/set_role_pref`,
  enum `AUTO`=0/`HUB`=1/`ROVER`=2) selects the boot role (§ Boot flow). Unset or
  unrecognized → `AUTO`, so stale/garbage NVS never wedges an unknown role.

## Hardware-earned traps (2026-07-04, ESP32-C3 + Pi hub)
- **~~zenoh-pico has no usrpwd~~ → RESOLVED by the MQTT port (2026-07-09), then
  the identity model it enabled was itself RETIRED (2026-07-13).** This was
  the deciding scar: zenoh-pico declares `Z_CONFIG_USER/PASSWORD_KEY` but no
  transport code consumes them, so a per-robot MCU identity was impossible (a
  usrpwd router rejected the session in ~200 ms) — esp-mqtt's native
  username/password support is why the rover shipped on MQTT, not Zenoh.
  That capability turned out to matter for exactly one identity: the classroom
  redesign (2026-07-13) dropped per-robot credentials entirely — a robot's
  name is a topic address, not something MQTT auth gates — and kept
  username/password for `instructor` alone, gating only `fleet/estop`. The
  rover itself now connects with no MQTT auth at all (`rover_role.c`).
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
  `name` (post-join identity — no password field; a name is a topic address,
  not a credential, confirmed 2026-07-13), `mpins` (6-byte motor-pin
  blob, a custom-wired chassis), `role` (boot role, § Identity), `hubpin`
  (optional exact-SSID hub lock — rogue-hub guard, set via cmd/config
  `{"hub":…}`, `rover_hub_admits`). All optional — absent falls back to the
  compile-time / AUTO default.

## Status & design history
Live state (validated-on-hardware, open threads): the **pinned tracker,
robot#4**. Narrative: git log. What can't be regenerated from the code — the
chosen-against:

- **Election → claim-by-reboot → always-APSTA (2026-07-09).** The distributed
  election (grace window + MAC jitter + lowest-MAC tiebreak + abdication;
  shipped `5b5b49d`, deleted `2e32956`) auto-picked one shared ESP hub among
  student boards — imitating the Pi in software, on worse hardware, at the
  design's highest complexity. Its successor, claim-by-reboot (`RTC_NOINIT`
  flag), existed only to dodge a live STA→APSTA radio switch, and caused two
  hardware bugs (the `RTC_DATA` wipe reboot-loop; the pi-watch stack panic).
  Always-APSTA removed the switch, so both died: home↔classroom is a runtime
  re-point of the broker URI, and the room's topology is explicit (a hub exists
  or it doesn't), never emergent. Don't re-propose coordination between boards.
- **Per-board-AP-vs-shared-hub dichotomy dissolved (hub#3, closed 2026-07-10)** —
  always-APSTA runs both topologies, split by hub presence.
- **The AP drops while joined to a hub (2026-07-15)** — hub#3's deferred
  mitigation, spent. It was gated on *measuring* the beacon cost; what actually
  bought it was the **mental model**: the room should be "everything is on the
  hub", not "everything is on the hub, and also each robot is its own network".
  A board that is simultaneously a hub client and its own AP is exactly the
  emergent third state the entry above says the topology must never have — the
  principle was already written down, and the AP was quietly violating it.
  Two supporting costs, both real but neither sufficient alone: beacons land on
  the **hub's own channel** (single radio → the AP follows the STA, so they
  contend with the drive path itself), and NAPT made each rover a second
  password-less door into the classroom network (*not* an escalation — the hub's
  AP is open too — but it made "connect to the hub" a suggestion rather than a
  fact). **Chosen-against: dropping the AP in the `BOARD_NET_REMOTE` case too**
  (stored home network + stored broker). Same redundancy argument, but the
  classroom is where the cost concentrates and the model is taught; at home one
  or two boards cost nothing and the AP is the recovery channel. Narrow the
  change to the case that earned it.
  **Chosen-against: keeping the AP as an escape hatch on a hub-joined board.**
  It isn't one — a misjoined board is reachable at `rover-<id>.local` on the
  hub's LAN, and the true dead-end (an ESP32-CAM, no BOOT button, joined to a
  hub you don't control) is recovered by powering it up out of the hub's range.
- **OS-native captive-portal onboarding, rebuilt end to end (2026-07-13 →
  2026-07-14, all live-diagnosed against real boards + real phones, not
  designed ahead of the bugs).** Rover→hub auto-discovery needs no human and
  is untouched; the one config that stays human-driven is a board's own
  uplink Wi-Fi in the island scenario. `wifi_portal.c` answers the OS probes
  (`/hotspot-detect.html`, `/generate_204`, `/connecttest.txt`, `/ncsi.txt`)
  via `probe_redirect`; a wildcard DNS responder (`dns_server.c`, :53) makes
  those probes resolve to this board at all.
  - **Chosen-against #1 — 302 straight to `/`.** `board_run`'s self-hosting
    path re-registers `/` to the live dashboard almost immediately, so a real
    board was caught rendering the FULL dashboard inside iOS's sandboxed
    Captive Network Assistant sheet — whose `localStorage` never reaches real
    Safari, silently losing any sign-in made there. Fixed: probes now 302 to
    `/welcome`, a page `ws_mqtt_bridge.c`/`landing_get` never touch.
  - **Chosen-against #2 — per-source-IP accept tracking.** The first
    Accept-flip cut (port of `hub/pi/src/bin/hubd.rs`'s design) keyed
    `captive_accepted()` by `httpd_req_to_sockfd`+`getpeername`. Live-tested:
    every request read back peer IP `0.0.0.0` on this httpd stack, so the
    flip silently never engaged. Replaced with ONE global accept flag + a
    15-minute idle window — a rover's own AP overwhelmingly serves one phone
    at a time, and the open ACL already gives every client full read+write
    regardless, so a flag shared across clients costs nothing real.
  - **Chosen-against #3 — flipping to "genuine success" unconditionally.**
    That's iOS's OWN signal to trust a Wi-Fi network for real internet
    routing. Live-tested on a pure-island board (no uplink at all): the flip
    fired, the captive sheet dismissed correctly, and the phone's OTHER apps
    lost internet, because we'd told iOS this network had it when it didn't.
    `captive_accepted()` now also requires `board_has_uplink()` (live, not
    cached) — the flip only ever fires when the claim is actually true.
  - **The real fix for "no real uplink" wasn't a better apology — it was
    onboarding.** A captive network that will never have internet isn't the
    common case any commercial captive portal handles (a hotel's gate is
    temporary; ours was being treated as permanent). `/welcome` now embeds
    the same scan/join flow as the dashboard's Set-up-Wi-Fi panel
    (`/wifi/scan`, `/wifi/connect`) directly in the captive sheet: pick a
    network there and Continue only appears once there's something true to
    tell iOS — exactly like a hotel portal releasing the sheet after sign-in.
    "Skip — drive it without internet" stays as the honest fallback (no
    flip, ever) for the genuine island case.
  - **DHCP option 114 / RFC 8910 (2026-07-14).** `wifi_apsta_up` advertises a
    `captive-portal-api` URI in the DHCP offer itself — iOS 14+/macOS Big
    Sur+/Android 11+ can read captivity straight from the DHCP handshake
    instead of guessing from a probe redirect. `GET /captive-portal-api`
    serves the real RFC 8908 JSON shape (`application/captive+json`), gated
    on the same `captive_accepted()`. Apple's spec wants this over TLS, which
    no private classroom AP can offer — a strict client may just ignore the
    plain-HTTP URI and fall back to the probe flow above, which still works;
    this is additive, not a replacement.
  - **Second pass, research-audited (2026-07-14).** A survey of OS captive
    clients + canonical implementations (ESP-IDF example, WBA behavior wiki,
    AOSP NetworkMonitor, Microsoft NCSI docs) confirmed the probe-intercept
    architecture is the only universal no-app path (RFC 8908 requires a
    public-CA TLS cert no offline AP can have) and drove four fixes:
    - **DNS goes truthful with the uplink** (`dns_server.c`): a client whose
      DHCP lease predates the uplink keeps THIS board as resolver for up to
      the 120-min lease, so wildcard-hijacking forever meant "trusted
      network, every website is the robot." The responder now forwards
      queries to the uplink's real DNS whenever `board_has_uplink()`, hijacks
      only while offline — checked per query, immune to lease races.
    - **Continue is a real navigation** (`location.href='/welcome?done=1'`),
      not a fetch+DOM swap: the CNA re-runs its captivity probe only on
      full-page loads — an AJAX-only accept leaves the sheet stuck on Cancel.
      (httpd matches handlers on the path component only, so `?done=1`
      reaches the `/welcome` handler.)
    - **Catch-all 404 → 302** (`not_found_handler`): a probe path we don't
      know (Windows `/redirect`, Firefox `canonical.html`, OEM variants)
      used to 404, which reads as "no internet", not "captive" — no sheet.
      Host-gated: public DNS names redirect, IP-literal/`.local`/bare hosts
      keep honest 404s so the dashboard's own API misses never bounce.
    - **Redirect Locations are absolute IP-literals** (`s_welcome_url`) —
      Android's login WebView can't resolve `.local`, and in-garden DNS is
      the top cross-OS failure mode.
    - `/welcome` polls `/wifi/status` (3 s) instead of checking once: after a
      connect-reboot the sheet reopens mid-join and a single check showed the
      picker again; now it walks connecting → reconnecting → Continue, with a
      7-poll fallback back to the picker.
  - **Third pass — a DHCP lease is not internet (2026-07-14, found live on a
    university visitor network).** With "uplink" meaning "STA got an
    address", a board joined to a venue whose network has its OWN captive
    gate reported `full`, forwarded the phone's probes into the venue's
    walled garden, and the student's captive sheet rendered the venue's SSO
    where /welcome should be. Fix is Pi parity: the board now runs hubd's
    probe_uplink (same endpoint, same none/portal/full verdicts, same
    debounce — hub/pi/src/bin/hubd.rs) and everything gates on the verdict,
    not the lease. In `portal` state the DNS responder goes mixed: probe
    hostnames hijack (the sheet stays ours), everything else forwards — so
    /welcome can link the venue's captured gate URL through the NAT, and one
    tap-through authorizes the board's venue-side MAC for the whole
    classroom. The on-uplink DHCP-DNS switch (`ap_offer_dns_from_sta`) is
    deleted: an offer can't be un-offered, and it was handing re-leasing
    phones the venue's resolver directly, bypassing the whole portal. Full
    chain verified live the same day: `uplink verdict: portal — venue gate at
    https://dukereg.duke.edu/login/aup`, probes hijacked, /welcome served.
  - **Known limitations, researched and accepted (2026-07-14), don't re-fix:**
    some Samsung builds treat a 302'd `generate_204` as a dead network and
    never open the portal (answering 204 honestly would lie to every other
    Android — accepted); iOS 26 has an open regression where the auto-sheet
    errors on exactly this DNS-wildcard pattern (Apple DevForums thread
    805035 — the manual `http://192.168.99.1` / rover.local path is the
    fallback); a live `/wifi/scan` while the AP serves the sheet briefly
    drops the radio (can cache scan results if it ever bites); and on iOS,
    "Use Without Internet" disables Auto Login for that SSID — the sheet
    deliberately never reappears on rejoin. That last one is Apple's design,
    not a firmware bug: a board whose sheet "stopped showing up" was skipped
    once on that phone.
  - The WS-bridge socket leak this whole investigation surfaced along the way
    (a departing station's dashboard bridge never got reclaimed, and enough
    silent leaks wedged the whole board) is `ws_mqtt_bridge.c`'s own
    chosen-against — see that file's `ws_bridge_reap_all`.
  - On a locked-down device, or an OS that never fires the probe or reads
    DHCP 114, all of this is a no-op: it still just visits `/wifi` manually.

History ladder (details: git log): v2 zenoh + BLE provisioning → v3/v4 MQTT port
+ motor drive → v5 BLE removed → v6 unified always-APSTA image.
(`DESIGN-unified.md` folded into this section + README, 2026-07-10.)
