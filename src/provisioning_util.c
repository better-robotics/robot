#include "provisioning_util.h"
#include <stdio.h>
#include <string.h>
#include "roles.h"   /* HUB_SSID_PREFIX — the one definition of what a hub is named */

/* A stored broker locator: mqtt://host:port. (Re-ported 2026-07-10 from the
 * zenoh-era tcp/host:port — the validator survived the MQTT port unported, so
 * rover_config_load wiped every stored mqtt:// locator as "corrupt" and the
 * stored-broker path could never fire.) */
bool rover_validate_locator(const char *s) {
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

void rover_format_robot_id(const uint8_t mac[6], char out[16]) {
    snprintf(out, 16, "rover-%02x%02x", mac[4], mac[5]);
}

bool rover_hub_admits(const char *ssid, const char *pin) {
    if (!ssid || strncmp(ssid, HUB_SSID_PREFIX, sizeof HUB_SSID_PREFIX - 1) != 0)
        return false;
    return !pin || !pin[0] || strcmp(ssid, pin) == 0;
}
