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
