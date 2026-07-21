#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_http_server.h"

/* The per-board Wi-Fi config panel (robot#2 / #17). A single always-on httpd on
 * the board's :80, brought up by board_run right after the AP, so a student can
 * join the open robot-<id> AP, browse robot.local, and set their home Wi-Fi —
 * the ISLAND case, which is the only one that needs this: a board with no hub
 * runs no dashboard until it self-brokers, and its uplink is the one thing that
 * can't be configured over a network it hasn't joined yet.
 *
 * The httpd stays up in every mode, but its REACH follows the AP. A hub-joined
 * board has no AP (hub_role.c board_ap_down), so this panel is then reachable
 * only over the hub's LAN at robot-<id>.local — which is fine, because such a
 * board takes its name and pins over MQTT (cmd/config) and has the hub's own
 * dashboard for everything else.
 *
 * Endpoints:
 *   GET  /wifi        the config page (self-contained HTML/JS/CSS) + status card.
 *   GET  /wifi/scan   JSON list of visible networks (via board_wifi_scan).
 *   POST /wifi/save   ssid=&pass= → NVS, then esp_restart to join it (a config-
 *                     apply reboot; always-APSTA means it comes right back up on
 *                     the new network — NOT the deleted mode-switch reboot).
 *   GET  /wifi/status live board state (board_status_json, roles.h) — what the
 *                     landing page and status card route on.
 *   GET  /            state-routing landing: holds while the board decides, sends
 *                     a classroom robot to the hub's dashboard, reloads into the
 *                     local dashboard once start_ws_mqtt_bridge takes over /. */
void wifi_portal_start(void);

/* The :80 handle the portal owns (NULL only if wifi_portal_start failed/hasn't
 * run — both entry points call it before start_ws_mqtt_bridge). The bridge
 * registers its dashboard onto this shared server rather than fighting for :80;
 * NULL means "no portal, start your own :80" (a defensive fallback). */
httpd_handle_t wifi_portal_httpd(void);

/* Forget the captive Accept of any device that has left the AP past the grace
 * window, so its next join is greeted with /welcome again (the Pi's per-visit
 * behaviour). Safe to call on any board — a no-op when no AP is up. Driven by
 * the uplink-probe loop; see wifi_portal.c for the presence-reaper rationale. */
void captive_reap_absent(void);

/* Has this AP client (IPv4, network byte order) tapped Continue on /welcome and
 * not yet been reaped for absence? The captive Accept table, keyed per device —
 * captive_nat.c reads it as the sole bypass to its packet-layer capture, so an
 * accepted client's traffic flows to the real net while an un-greeted one's is
 * still walled onto /welcome. 0 (the getpeername-failure sentinel) is never
 * accepted. */
bool captive_accepted(uint32_t ip);

#endif
