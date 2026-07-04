#include "rover_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "provisioning_util.h"

#define NS "rover"

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    return nvs_get_str(h, key, out, &len) == ESP_OK && len > 0;
}

bool rover_config_load(char ssid[33], char pass[65], char locator[65]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = get_str(h, "ssid", ssid, 33) && get_str(h, "locator", locator, 65);
    size_t pl = 65;
    if (nvs_get_str(h, "pass", pass, &pl) != ESP_OK) pass[0] = 0;   // empty = open net
    nvs_close(h);
    return ok && rover_validate_locator(locator);
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
    return rover_config_load(s, p, l);
}
