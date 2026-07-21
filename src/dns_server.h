#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal wildcard DNS-over-UDP responder for the board's own AP (`robot-<id>`
 * or `hub-<id>` — same AP, different SSID prefix; both served by
 * wifi_portal.c). Answers EVERY A-record query with the AP's own IP, so a
 * joining device's OS captive-portal probe resolves to this board and its
 * captive-probe HTTP handlers (wifi_portal.c) can redirect it to "/" —
 * triggering the OS's native captive-portal auto-popup (Apple/Android/
 * Windows) instead of requiring a manual http://robot.local visit.
 *
 * True wildcard is the correct scope here — unlike the Pi's dnsmasq, which is
 * scoped to specific OS probe domains against a real uplink, an ESP32 SoftAP
 * has no real uplink DNS of its own to defer to, so there is nothing
 * legitimate to answer differently for any other name.
 *
 * Starts a small FreeRTOS task and returns immediately. Call once per boot,
 * after the AP netif is up (wifi_portal_start does this).
 */
void dns_server_start(void);

/* Exported for captive_nat.c: the packet-layer capture needs to apply the
 * SAME probe-hostname policy this file's own task loop uses, off the SAME
 * list — never a second copy that can drift. */
bool probe_hostname(const char *q);
void question_name(const uint8_t *pkt, int len, char *out, size_t outlen);

#endif
