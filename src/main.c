/*
 * app_main — the boot dispatcher for the unified image.
 *
 * One firmware, decided from role_pref in NVS (DESIGN-unified.md § Always-APSTA):
 *
 *   role_pref = hub   → tier 2: a dedicated professor hub (hub_role_run) — a
 *                       hub-* AP + broker, no drive.
 *   role_pref = auto  → (default) the normal board (board_run, self_broker_ok=1):
 *                       always APSTA — its own rover-<id> AP + STA uplink. Joins a
 *                       hub → drives off it; no hub → runs a local broker and
 *                       drives itself (home/island). No mode-switch reboot.
 *   role_pref = rover → the same board path, but pinned NOT to self-broker
 *                       (board_run, self_broker_ok=0): it keeps looking for a hub.
 *
 * There is no longer a self-hub claim-by-reboot: the board comes up in APSTA and
 * never switches radio mode, so home↔classroom is runtime state, not a boot role.
 * Shared one-time init (NVS) lives here so neither path repeats it.
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
        ESP_LOGI(TAG, "role: hub (role_pref, tier 2) — dedicated professor hub, no drive");
        hub_role_run();     /* never returns (blocks in the broker) */
    }
    ESP_LOGI(TAG, "role: board%s — always-APSTA (own AP + drive; local broker when no hub)",
             role == ROLE_AUTO ? " (auto)" : " (rover-pinned)");
    board_run(role == ROLE_AUTO);   /* AUTO may self-broker (island); ROVER never does. never returns */
}
