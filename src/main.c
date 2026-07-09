/*
 * app_main — the boot dispatcher for the unified rover image.
 *
 * One firmware, two roles (hub self-election, hub/esp32/DESIGN-unified.md): a
 * board is normally a rover (esp-mqtt client + motor drive), but the same image
 * can boot as the on-chip hub (AP + broker + WS bridge). Which one it becomes is
 * decided here from role_pref in NVS:
 *
 *   role_pref = hub   → always the hub  (hub_role_run)
 *   role_pref = rover → always a rover  (rover_role_run)
 *   role_pref = auto  → (default) a rover today. The scan-elect "no hub-* in
 *                       range → become the hub" is the election protocol (step 3
 *                       of the self-election build); until it lands, auto == rover
 *                       and a device is made a hub explicitly (the dashboard /
 *                       config page sets role_pref = hub). So this dispatcher is
 *                       the mechanical role switch, not yet the election.
 *
 * Shared one-time init (NVS) lives here so neither role repeats it; each role
 * then brings up its own radio and never returns.
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "rover_config.h"
#include "roles.h"

static const char *TAG = "boot";

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) {   /* first boot / version bump → erase + retry */
        nvs_flash_erase();
        nvs_flash_init();
    }

    rover_role_pref_t role = rover_config_load_role_pref();
    if (role == ROLE_HUB) {
        ESP_LOGI(TAG, "role: hub (role_pref) — starting the on-chip hub");
        hub_role_run();     /* never returns */
    }
    ESP_LOGI(TAG, "role: rover%s", role == ROLE_AUTO ? " (auto)" : " (role_pref)");
    rover_role_run();       /* never returns */
}
