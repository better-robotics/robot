#include "robot_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "provisioning_util.h"
#include "roles.h"   /* HUB_SSID_PREFIX — hub-pin plausibility check */

#define NS "robot"

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    return nvs_get_str(h, key, out, &len) == ESP_OK && len > 0;
}

void robot_config_load(char ssid[33], char pass[65], char locator[65]) {
    ssid[0] = pass[0] = locator[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "ssid", ssid, 33);
    get_str(h, "pass", pass, 65);   // empty = open net
    if (get_str(h, "locator", locator, 65) && !robot_validate_locator(locator))
        locator[0] = 0;             // corrupt entry degrades to "derive from gateway"
    nvs_close(h);
}

/* Key is "profpass"; NVS keys cap at 15 chars. Loads to "" when unset — the
 * caller falls back to the compile-time default, so an un-provisioned board is
 * unchanged. */
void robot_config_load_operator_pass(char pass[65]) {
    pass[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "profpass", pass, 65);
    nvs_close(h);
}

/* "" ERASES it (back to the compile-time default) rather than storing an empty
 * password — an empty stored secret would admit every client as operator, the
 * exact silent failure a -D${sysenv} build flag has when the var is unset. */
esp_err_t robot_config_set_operator_pass(const char *pass) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if (!pass || !pass[0]) {
        e = nvs_erase_key(h, "profpass");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;   /* already absent = success */
    } else if (strlen(pass) > 64) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    } else {
        e = nvs_set_str(h, "profpass", pass);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t robot_config_set_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass ? pass : "");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* "Forget this network" (dashboard's Set-up-Wi-Fi panel) — erase the stored
 * uplink so the board reboots back to a fresh island (no venue/home network,
 * same as a board that's never been configured). Deliberately narrower than
 * a full NVS wipe: name, role, motor pins, and hub pin all survive — those
 * are identity/hardware facts, not the uplink. */
esp_err_t robot_config_clear_wifi(void) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "ssid");   /* absent key is fine — a fresh board has neither */
    nvs_erase_key(h, "pass");
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* No password: a robot's name is a topic address, not a credential — the
 * hub's own Wi-Fi is the classroom's real boundary (CONTRACT.md § Discovery
 * & isolation, confirmed 2026-07-13). */
void robot_config_load_identity(char name[33]) {
    name[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "name", name, 33);
    nvs_close(h);
}

esp_err_t robot_config_set_identity(const char *name) {
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;   // a name is required
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "name", name);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t robot_config_clear_identity(void) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "name");   /* absent key is fine — a fresh board reprovisions cleanly */
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// Stored as one 6-byte blob under "mpins" — GPIO numbers fit in a byte and the
// set is atomic (all six or none).
bool robot_config_load_motor_pins(int pins[6]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t raw[6]; size_t len = sizeof raw;
    bool ok = nvs_get_blob(h, "mpins", raw, &len) == ESP_OK && len == 6;
    nvs_close(h);
    if (ok) for (int i = 0; i < 6; i++) pins[i] = raw[i];
    return ok;
}

esp_err_t robot_config_set_motor_pins(const int pins[6]) {
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

void robot_config_load_hub_pin(char pin[33]) {
    pin[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "hubpin", pin, 33);
    nvs_close(h);
}

esp_err_t robot_config_set_hub_pin(const char *pin) {
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
robot_role_pref_t robot_config_load_role_pref(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ROLE_AUTO;
    uint8_t v = ROLE_AUTO;
    nvs_get_u8(h, "role", &v);   // leaves v = AUTO if the key is absent
    nvs_close(h);
    return (v == ROLE_HUB || v == ROLE_ROBOT) ? (robot_role_pref_t)v : ROLE_AUTO;
}

esp_err_t robot_config_set_role_pref(robot_role_pref_t role) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "role", (uint8_t)role);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
