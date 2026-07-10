#ifndef ROLES_H
#define ROLES_H

#include <stdbool.h>

/* Entry points of the unified image (DESIGN-unified.md § Always-APSTA). app_main
 * (main.c) reads role_pref from NVS and calls one; each brings up its own radio
 * and never returns. */

/* The normal board (tiers 1 + 3), always APSTA — own open rover-<id> AP + STA
 * uplink, no mode switch ever. Joins a hub-* → drives off it (classroom); no hub
 * → runs a local broker and drives itself (home/island). self_broker_ok = AUTO
 * (may island); false = ROVER-pinned (keeps looking, never self-brokers).
 * Defined in hub_role.c (which owns the Wi-Fi + broker services). */
void board_run(bool self_broker_ok);

/* Tier 2: a dedicated professor hub — hub-* AP + broker + NAT, no drive
 * (hub_role.c). Chosen via role_pref=HUB. */
void hub_role_run(void);

/* rover_client_run — the MQTT client + motor-drive loop with NO Wi-Fi setup of
 * its own (rover_role.c). board_run passes the broker: the DHCP gateway when
 * joined to a hub, or mqtt://127.0.0.1:1883 when the board is its own island.
 * Assumes networking is already up; returns on a dead session, never reboots.
 * rover_button_start arms the recover button (hold to reboot). */
void rover_client_run(const char *broker_uri);
void rover_button_start(void);

/* SSID classification shared by discovery and Pi-watch (hub_role.c). Any open
 * "hub-*" is a hub to join; "hub-pi-*" additionally marks the Pi, which a self-
 * hub (island) board ALWAYS yields to (DESIGN-unified.md § Pi-preference — the Pi
 * is the preferred hub; peer ESP islands are left alone). */
#define HUB_SSID_PREFIX     "hub-"
#define HUB_PI_SSID_PREFIX  "hub-pi-"

/* A scanned network, deduped by SSID (strongest kept). A flat struct so the Wi-Fi
 * config panel (wifi_portal.c) can list networks without pulling in esp_wifi.h.
 * board_wifi_scan is implemented in hub_role.c because it owns the radio + the
 * s_want_connect reconnect gate — it saves/restores that gate around the scan so a
 * panel scan can't silently kill the STA auto-reconnect (robot#1). Returns the
 * count written (≤ max), 0 on failure (e.g. a scan already in flight). */
typedef struct { char ssid[33]; signed char rssi; bool open; } board_ap_t;
int board_wifi_scan(board_ap_t *out, int max);

/* Camera (camera.c). Inits the OV2640 and serves MJPEG at :81/stream — a no-op
 * unless built with HAS_CAMERA (the esp32cam board). Call after Wi-Fi is up
 * (connection-first: camera fits in what memory is left, or fails loudly).
 * camera_running() reports whether init succeeded, so sys telemetry advertises
 * the stream only when it's actually live. */
void camera_start(void);
bool camera_running(void);

#endif
