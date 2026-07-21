#ifndef ROLES_H
#define ROLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Entry points of the unified image (README "How a board decides what to be").
 * app_main
 * (main.c) reads role_pref from NVS and calls one; each brings up its own radio
 * and never returns. */

/* The normal board (tiers 1 + 3), APSTA at boot — own open rover-<id> AP + STA
 * uplink. Joins a hub-* → drives off it AND drops its own AP for as long as it
 * stays a hub client (classroom: one network in the room, the hub's); no hub →
 * keeps the AP and runs a local broker, driving itself (home/island).
 * self_broker_ok = AUTO (may island); false = ROVER-pinned (keeps looking, never
 * self-brokers). Defined in hub_role.c (which owns the Wi-Fi + broker services). */
void board_run(bool self_broker_ok);

/* Tier 2: a dedicated operator hub — hub-* AP + broker + NAT, no drive
 * (hub_role.c). Chosen via role_pref=HUB. */
void hub_role_run(void);

/* rover_client_run — the MQTT client + motor-drive loop with NO Wi-Fi setup of
 * its own (rover_role.c). board_run passes the broker: the DHCP gateway when
 * joined to a hub, or mqtt://127.0.0.1:1883 when the board is its own island.
 * Assumes networking is already up; returns on a dead session, never reboots.
 * rover_button_start arms the recover button (hold to reboot). */
void rover_client_run(const char *broker_uri);
void rover_button_start(void);

/* Milliseconds since the last drive (pwm) command, or INT64_MAX if none this
 * boot. hub_watch (hub_role.c) reads it to skip its yield-scan while a board is
 * being actively driven — an active scan briefly interrupts AP+STA, so an island
 * only looks for a late-booting hub when idle, never mid-drive. */
int64_t rover_ms_since_drive(void);

/* The one gated identity, checked in one place (hub_role.c): NVS-stored password
 * first, compile-time OPERATOR_PASS second, false for NULL. Used by the
 * broker's session auth and by POST /ota (ota_update.c) — the same secret gates
 * "could take the room down over MQTT" and "could take the room down by
 * reflashing it", so there is nothing to rotate twice. */
bool board_operator_pass_ok(const char *given);

/* SSID classification shared by discovery and hub-watch (hub_role.c): any open
 * "hub-*" is a hub to join (peer islands advertise rover-<id>, never hub-*). */
#define HUB_SSID_PREFIX     "hub-"

/* Fresh boards' pool name, normally set per-env via -DROVER_NAME
 * (platformio.ini). No password: a name is a topic address, not a
 * credential (CONTRACT.md § Discovery & isolation) — every hub admits every
 * name with no auth, so there's nothing left for the two sides of an island
 * to share but the default string itself. */
#ifndef ROVER_NAME
#define ROVER_NAME "unassigned"
#endif

/* Board network state, published by hub_role.c at each broker decision and served
 * as JSON by GET /wifi/status (wifi_portal.c). The landing page at / and the
 * config panel's status card route on it — `dash` is the actionable field: where
 * the drive dashboard for THIS board lives right now ("" while undecided, "/"
 * once this board serves it, "http://<ip>/" when it's at a hub or stored broker). */
typedef enum {
    BOARD_NET_SEARCHING = 0,  /* pre-decision: scanning for a hub / trying stored */
    BOARD_NET_HUB,            /* STA joined a hub-* — dashboard at the DHCP gateway */
    BOARD_NET_REMOTE,         /* stored network + explicit locator (e.g. a home Pi) */
    BOARD_NET_LOCAL,          /* this board serves broker + dashboard itself */
} board_net_state_t;
void board_net_state_set(board_net_state_t st, const char *uplink_ssid, const char *dash);
void board_uplink_ssid_json(char out[65]);       /* sys beacon "net" field */
int  board_status_json(char *buf, size_t len);   /* → bytes written (snprintf) */
/* Uplink verdict — the Pi hubd's probe_uplink vocabulary (hub/pi/src/bin/
 * hubd.rs), same probe: fetch a known 204 endpoint the way phones do.
 * 204 → FULL; any other HTTP answer → PORTAL (something answered in the
 * endpoint's place: the venue's own captive gate — a DHCP lease is NOT
 * internet, learned live on a university visitor network 2026-07-14);
 * no answer / no lease → NONE. */
typedef enum { BOARD_UPLINK_NONE, BOARD_UPLINK_PORTAL, BOARD_UPLINK_FULL } board_uplink_t;
board_uplink_t board_uplink(void);
void board_portal_url(char out[160]);   /* the venue gate's Location, "" unknown */
bool board_has_uplink(void);   /* verdict == FULL — the signal /wifi/status's
                                 * "uplink":"full" reports; wifi_portal.c's captive-portal
                                 * Accept flip gates on this so it never tells iOS a
                                 * board without working internet (island OR venue-walled)
                                 * has it (2026-07-14). */

/* A scanned network, deduped by SSID (strongest kept). A flat struct so the Wi-Fi
 * config panel (wifi_portal.c) can list networks without pulling in esp_wifi.h.
 * board_wifi_scan is implemented in hub_role.c because it owns the radio + the
 * s_want_connect reconnect gate — it saves/restores that gate around the scan so a
 * panel scan can't silently kill the STA auto-reconnect (robot#1).
 * Returns the count written (≤ max); 0 = scanned and saw nothing; **-1 = the
 * radio was busy** (a hub-watch scan in flight; already retried 3× internally).
 * Callers MUST keep those apart. They were ONE value (0) until 2026-07-17, which
 * is how the picker came to tell a student in a full apartment block "No networks
 * found." — a collision reported as a fact about the room. */
typedef struct { char ssid[33]; signed char rssi; bool open; } board_ap_t;
int board_wifi_scan(board_ap_t *out, int max);

/* Live uplink re-dial (wifi_portal's POST /wifi/connect). Only the dedicated
 * hub role honors it — a hub's AP never drops (it is the room's network), so new
 * credentials can apply WITHOUT the config-apply reboot that would drop the AP
 * under the phone driving the panel. Returns false outside hub mode (caller falls
 * back to the reboot). */
bool board_wifi_redial(const char *ssid, const char *pass);

/* Trial-join (wifi_portal's POST /wifi/connect, board/rover mode): attempt
 * the credentials live — the AP and the portal page stay up — blocking up
 * to ~20 s for an IP. NULL = verified (caller persists + reboots to apply);
 * otherwise a short human verdict ("wrong password?"), with the previous
 * uplink re-dialed best-effort. Validates BEFORE the config-apply reboot so
 * bad credentials are never saved and rebooted into. */
const char *board_wifi_try_join(const char *ssid, const char *pass);

/* Camera (camera.c). Inits the OV2640 and serves MJPEG at :81/stream — a no-op
 * unless built with HAS_CAMERA (the esp32cam board). Call after Wi-Fi is up
 * (connection-first: camera fits in what memory is left, or fails loudly).
 * camera_running() reports whether init succeeded, so sys telemetry advertises
 * the stream only when it's actually live. */
void camera_start(void);
bool camera_running(void);

#endif
