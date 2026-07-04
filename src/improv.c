#include "improv.h"
#include <string.h>

uint8_t improv_checksum(const uint8_t *b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
}

static int is_control(unsigned char c) { return c < 0x20 || c == 0x7f; }

static improv_parse_t parse_wifi(const uint8_t *d, size_t n, improv_command_t *out) {
    if (n < 1) return IMPROV_ERR_INVALID;
    size_t sl = d[0];
    if (1 + sl >= n) return IMPROV_ERR_INVALID;        // need pass_len byte after ssid
    size_t pl = d[1 + sl];
    if (1 + sl + 1 + pl != n) return IMPROV_ERR_INVALID;
    if (sl == 0 || sl > 32) return IMPROV_ERR_INVALID;
    if (pl > 64) return IMPROV_ERR_INVALID;
    memcpy(out->ssid, d + 1, sl); out->ssid[sl] = 0;
    memcpy(out->pass, d + 2 + sl, pl); out->pass[pl] = 0;
    if (out->ssid[0] == '-') return IMPROV_ERR_INVALID;             // nmcli flag-smuggle
    for (size_t i = 0; i < sl; i++) if (is_control((unsigned char)out->ssid[i])) return IMPROV_ERR_INVALID;
    for (size_t i = 0; i < pl; i++) if (is_control((unsigned char)out->pass[i])) return IMPROV_ERR_INVALID;
    out->cmd = IMPROV_CMD_SEND_WIFI;
    return IMPROV_OK;
}

improv_parse_t improv_parse_command(const uint8_t *pkt, size_t n, improv_command_t *out) {
    memset(out, 0, sizeof *out);
    if (n < 3) return IMPROV_ERR_INVALID;
    uint8_t cmd = pkt[0];
    size_t data_len = pkt[1];
    if (n != data_len + 3) return IMPROV_ERR_INVALID;
    if (improv_checksum(pkt, n - 1) != pkt[n - 1]) return IMPROV_ERR_INVALID;
    const uint8_t *data = pkt + 2;
    switch (cmd) {
        case IMPROV_CMD_SEND_WIFI: return parse_wifi(data, data_len, out);
        case IMPROV_CMD_SCAN_WIFI: out->cmd = cmd; return IMPROV_OK;
        case IMPROV_CMD_DEVICE_INFO: out->cmd = cmd; return IMPROV_OK;
        case IMPROV_CMD_IDENTIFY: out->cmd = cmd; return IMPROV_OK;
        default: return IMPROV_ERR_UNKNOWN_CMD;
    }
}

size_t improv_encode_result(uint8_t cmd, const char *const *strings, size_t count,
                            uint8_t *out, size_t cap) {
    if (cap < 3) return 0;
    size_t i = 0;
    out[i++] = cmd; out[i++] = 0;                       // length filled below
    for (size_t s = 0; s < count; s++) {
        size_t sl = strlen(strings[s]);
        if (i + 1 + sl + 1 > cap) return 0;
        out[i++] = (uint8_t)sl;
        memcpy(out + i, strings[s], sl); i += sl;
    }
    out[1] = (uint8_t)(i - 2);
    out[i] = improv_checksum(out, i); i++;
    return i;
}
