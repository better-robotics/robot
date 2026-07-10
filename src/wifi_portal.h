#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include "esp_http_server.h"

/* The per-board Wi-Fi config panel (robot#2 / #17). A single always-on httpd on
 * the board's :80, brought up by board_run right after the AP, so a student can
 * join the open rover-<id> AP, browse rover.local, and set their home Wi-Fi in
 * ANY mode — including a classroom rover that runs no broker/dashboard of its own.
 *
 * Endpoints:
 *   GET  /wifi        the config page (self-contained HTML/JS/CSS).
 *   GET  /wifi/scan   JSON list of visible networks (via board_wifi_scan).
 *   POST /wifi/save   ssid=&pass= → NVS, then esp_restart to join it (a config-
 *                     apply reboot; always-APSTA means it comes right back up on
 *                     the new network — NOT the deleted mode-switch reboot).
 *   GET  /            302 → /wifi, so a bare rover.local lands on setup. When the
 *                     board islands, start_ws_mqtt_bridge unregisters this redirect
 *                     and serves the drive dashboard at / instead. */
void wifi_portal_start(void);

/* The :80 handle the portal owns (NULL if wifi_portal_start hasn't run — e.g. the
 * tier-2 hub, which never calls board_run). start_ws_mqtt_bridge registers its
 * dashboard onto this shared server rather than fighting for :80; NULL means "no
 * portal, start your own :80" (the hub path). */
httpd_handle_t wifi_portal_httpd(void);

#endif
