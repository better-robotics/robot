#include "provisioning_util.h"
#include <stdio.h>
#include <string.h>
#include "roles.h"   /* HUB_SSID_PREFIX — the one definition of what a hub is named */

/* A stored broker locator: mqtt://host:port. (Re-ported 2026-07-10 from the
 * zenoh-era tcp/host:port — the validator survived the MQTT port unported, so
 * robot_config_load wiped every stored mqtt:// locator as "corrupt" and the
 * stored-broker path could never fire.) */
bool robot_validate_locator(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0 || n > 64) return false;
    if (strncmp(s, "mqtt://", 7) != 0) return false;
    const char *host = s + 7;
    const char *colon = strrchr(host, ':');
    if (!colon || colon == host) return false;           // need host and a port sep
    for (const char *p = host; p < colon; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return false;         // no space/control
    }
    const char *port = colon + 1;
    if (*port == 0) return false;
    long v = 0;
    for (const char *p = port; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        if (v > 65535) return false;
    }
    return v >= 1 && v <= 65535;
}

void robot_format_robot_id(const uint8_t mac[6], char out[16]) {
    snprintf(out, 16, "robot-%02x%02x", mac[4], mac[5]);
}

bool robot_hub_admits(const char *ssid, const char *pin) {
    if (!ssid || strncmp(ssid, HUB_SSID_PREFIX, sizeof HUB_SSID_PREFIX - 1) != 0)
        return false;
    return !pin || !pin[0] || strcmp(ssid, pin) == 0;
}

bool robot_host_is_local(const char *host) {
    if (!host || !host[0]) return true;         /* no Host → ours (a rebind names a host) */
    bool dotted = false, ip_literal = true;
    for (const char *p = host; *p; p++) {
        if (*p == '.') dotted = true;
        if ((*p < '0' || *p > '9') && *p != '.' && *p != ':') ip_literal = false;
    }
    if (!dotted || ip_literal) return true;     /* bare label, or a v4/v6 literal */
    size_t hl = strlen(host);                   /* case-insensitive ".local" suffix */
    if (hl >= 6) {
        static const char suf[6] = { '.', 'l', 'o', 'c', 'a', 'l' };
        const char *s = host + hl - 6;
        bool ok = true;
        for (int i = 0; i < 6; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != suf[i]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;                               /* a public dotted name — not ours */
}
