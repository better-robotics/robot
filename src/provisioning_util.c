#include "provisioning_util.h"
#include <stdio.h>
#include <string.h>

bool rover_validate_locator(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0 || n > 64) return false;
    if (strncmp(s, "tcp/", 4) != 0) return false;
    const char *host = s + 4;
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
