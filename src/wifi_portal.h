#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include "esp_http_server.h"

/* The per-board Wi-Fi config panel (robot#2 / #17). A single always-on httpd on
 * the board's :80, brought up by board_run right after the AP, so a student can
 * join the open rover-<id> AP, browse rover.local, and set their home Wi-Fi in
 * ANY mode — including a classroom rover that runs no broker/dashboard of its own.
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
 *                     a classroom rover to the hub's dashboard, reloads into the
 *                     local dashboard once start_ws_mqtt_bridge takes over /. */
void wifi_portal_start(void);

/* The :80 handle the portal owns (NULL only if wifi_portal_start failed/hasn't
 * run — both entry points call it before start_ws_mqtt_bridge). The bridge
 * registers its dashboard onto this shared server rather than fighting for :80;
 * NULL means "no portal, start your own :80" (a defensive fallback). */
httpd_handle_t wifi_portal_httpd(void);

#endif
