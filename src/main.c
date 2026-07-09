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
 *   role_pref = auto  → (default) a rover that self-elects: it boots the rover
 *                       path, and if no hub-* is in range it runs the election
 *                       (rover_role.c). Winning the election reboots with the
 *                       RTC flag below set, which lands here as an "elected hub"
 *                       boot — a clean AP+STA init, not a STA→APSTA switch.
 *
 * Shared one-time init (NVS) lives here so neither role repeats it; each role
 * then brings up its own radio and never returns.
 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "rover_config.h"
#include "roles.h"

static const char *TAG = "boot";

/* Transient "boot as hub this time" flag (hub self-election, roles.h). RTC slow
 * memory survives esp_restart — the election's claim reboot and the hub's
 * abdication reboot — but is re-initialized on power-on, so a power cycle always
 * re-runs the election. That's the point: an ESP hub must re-evaluate and yield
 * to a Pi that is now present, rather than persist as a second hub. */
#define HUB_BOOT_MAGIC  0x48554221u   /* "HUB!" */
static RTC_DATA_ATTR uint32_t s_hub_boot_flag;

void role_boot_as_hub(void) {
    s_hub_boot_flag = HUB_BOOT_MAGIC;
    esp_restart();          /* never returns; the flag is read on the next boot */
}

bool role_pending_hub_boot(void) {
    if (s_hub_boot_flag == HUB_BOOT_MAGIC) {
        s_hub_boot_flag = 0;   /* consume once — a later failure reboots into a fresh election */
        return true;
    }
    return false;
}

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) {   /* first boot / version bump → erase + retry */
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (role_pending_hub_boot()) {      /* this boot won an election (roles.h) */
        ESP_LOGI(TAG, "role: hub (elected) — starting the on-chip hub");
        hub_role_run(true);     /* elected → may abdicate to a Pi / lower-MAC hub; never returns */
    }

    rover_role_pref_t role = rover_config_load_role_pref();
    if (role == ROLE_HUB) {
        ESP_LOGI(TAG, "role: hub (role_pref) — starting the on-chip hub");
        hub_role_run(false);    /* forced → never abdicates (would re-force into a reboot loop) */
    }
    ESP_LOGI(TAG, "role: rover%s", role == ROLE_AUTO ? " (auto)" : " (role_pref)");
    rover_role_run();       /* never returns — self-elects if no hub-* is found (AUTO) */
}
