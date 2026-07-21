/*
 * app_main — the boot dispatcher for the unified image.
 *
 * One firmware, decided from role_pref in NVS (README "How a board decides
 * what to be"):
 *
 *   role_pref = hub   → tier 2: a dedicated operator hub (hub_role_run) — a
 *                       hub-* AP + broker, no drive.
 *   role_pref = auto  → (default) the normal board (board_run, self_broker_ok=1):
 *                       APSTA at boot — its own robot-<id> AP + STA uplink. Joins
 *                       a hub → drives off it AND drops its own AP; no hub → runs
 *                       a local broker and drives itself (home/island).
 *   role_pref = robot → the same board path, but pinned NOT to self-broker
 *                       (board_run, self_broker_ok=0): it keeps looking for a hub.
 *
 * There is no longer a self-hub claim-by-reboot: the board comes up in APSTA, so
 * home↔classroom is runtime state, not a boot role. The one live mode change is
 * APSTA→STA on a hub join (safe: subtractive, the STA keeps its association);
 * STA→APSTA never happens live — that way round is a clean restart (hub_role.c).
 * Shared one-time init (NVS) lives here so neither path repeats it.
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "device_log.h"
#include "robot_config.h"
#include "roles.h"

static const char *TAG = "boot";

void app_main(void) {
    /* FIRST, before even NVS: this installs the log hook, and anything logged
     * ahead of it is gone. The failures worth reading back off a crashed board
     * are boot failures, so every line from here on is the point. */
    device_log_init();

    if (nvs_flash_init() != ESP_OK) {   /* first boot / version bump → erase + retry */
        nvs_flash_erase();
        nvs_flash_init();
    }

    robot_role_pref_t role = robot_config_load_role_pref();
#ifdef FORCE_ROLE_HUB
    role = ROLE_HUB;   /* bench validation only (-DFORCE_ROLE_HUB): pin this board to
                        * the dedicated-hub path without needing the config panel */
#endif
    if (role == ROLE_HUB) {
        ESP_LOGI(TAG, "role: hub (role_pref, tier 2) — dedicated operator hub, no drive");
        hub_role_run();     /* never returns (blocks serving the zenoh fabric) */
    }
    ESP_LOGI(TAG, "role: board%s — APSTA at boot (own AP until a hub takes over; "
                  "local broker when no hub)",
             role == ROLE_AUTO ? " (auto)" : " (robot-pinned)");
    board_run(role == ROLE_AUTO);   /* AUTO may self-broker (island); ROBOT never does. never returns */
}
