#include "rover_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "provisioning_util.h"
#include "roles.h"   /* HUB_SSID_PREFIX — hub-pin plausibility check */

#define NS "rover"

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    return nvs_get_str(h, key, out, &len) == ESP_OK && len > 0;
}

void rover_config_load(char ssid[33], char pass[65], char locator[65]) {
    ssid[0] = pass[0] = locator[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "ssid", ssid, 33);
    get_str(h, "pass", pass, 65);   // empty = open net
    if (get_str(h, "locator", locator, 65) && !rover_validate_locator(locator))
        locator[0] = 0;             // corrupt entry degrades to "derive from gateway"
    nvs_close(h);
}

esp_err_t rover_config_set_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass ? pass : "");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t rover_config_set_locator(const char *locator) {
    if (!rover_validate_locator(locator)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "locator", locator);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool rover_config_is_complete(void) {
    char s[33], p[65], l[65];
    rover_config_load(s, p, l);
    return s[0] != 0;
}

void rover_config_load_identity(char user[33], char pass[65], char name[33]) {
    user[0] = pass[0] = name[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "user", user, 33);
    get_str(h, "mpass", pass, 65);
    get_str(h, "name", name, 33);
    nvs_close(h);
}

esp_err_t rover_config_set_identity(const char *user, const char *pass, const char *name) {
    if (!user || !user[0]) return ESP_ERR_INVALID_ARG;   // a team is required
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "user", user);
    if (e == ESP_OK) e = nvs_set_str(h, "mpass", pass ? pass : "");
    if (e == ESP_OK) e = nvs_set_str(h, "name", name ? name : "");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// Stored as one 6-byte blob under "mpins" — GPIO numbers fit in a byte and the
// set is atomic (all six or none).
bool rover_config_load_motor_pins(int pins[6]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t raw[6]; size_t len = sizeof raw;
    bool ok = nvs_get_blob(h, "mpins", raw, &len) == ESP_OK && len == 6;
    nvs_close(h);
    if (ok) for (int i = 0; i < 6; i++) pins[i] = raw[i];
    return ok;
}

esp_err_t rover_config_set_motor_pins(const int pins[6]) {
    uint8_t raw[6];
    for (int i = 0; i < 6; i++) {
        if (pins[i] < 0 || pins[i] > 33) return ESP_ERR_INVALID_ARG;  // 34-39 are input-only
        raw[i] = (uint8_t)pins[i];
    }
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "mpins", raw, sizeof raw);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void rover_config_load_hub_pin(char pin[33]) {
    pin[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "hubpin", pin, 33);
    nvs_close(h);
}

esp_err_t rover_config_set_hub_pin(const char *pin) {
    if (!pin) return ESP_ERR_INVALID_ARG;
    // "" clears; anything else must be a plausible hub SSID — a non-hub-* pin
    // would admit nothing and silently strand the board off every hub.
    if (pin[0] && (strlen(pin) > 32 ||
                   strncmp(pin, HUB_SSID_PREFIX, sizeof HUB_SSID_PREFIX - 1) != 0))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "hubpin", pin);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// Boot role for the unified image — one byte under "role". Unset or any value
// outside the enum degrades to AUTO, so a garbage/older NVS never wedges the
// dispatcher into an unknown role.
rover_role_pref_t rover_config_load_role_pref(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ROLE_AUTO;
    uint8_t v = ROLE_AUTO;
    nvs_get_u8(h, "role", &v);   // leaves v = AUTO if the key is absent
    nvs_close(h);
    return (v == ROLE_HUB || v == ROLE_ROVER) ? (rover_role_pref_t)v : ROLE_AUTO;
}

esp_err_t rover_config_set_role_pref(rover_role_pref_t role) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "role", (uint8_t)role);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
