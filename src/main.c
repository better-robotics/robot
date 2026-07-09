/*
 * app_main — the boot dispatcher for the unified rover image.
 *
 * One firmware, role tiers (DESIGN-unified.md § Direction change): a board is
 * normally a rover (esp-mqtt client + motor drive), but the same image can boot
 * as the on-chip hub (AP + broker + WS bridge). Which one it becomes is decided
 * here from role_pref in NVS:
 *
 *   role_pref = hub   → tier 2: always the hub, no drive  (hub_role_run(false))
 *   role_pref = rover → tier 1: always a rover            (rover_role_run)
 *   role_pref = auto  → (default) boots the rover path; if no hub-* is in range
 *                       it self-hubs (rover_role.c) by setting the RTC flag below
 *                       and rebooting, which lands here as a "self-hub" boot —
 *                       tier 3 (home mode): hub + local rover drive, a clean
 *                       AP+STA init rather than a STA→APSTA switch mid-boot.
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

/* Transient "boot as hub this time" flag (self-hub claim, roles.h). RTC slow
 * memory survives esp_restart — the self-hub claim reboot and the hub's Pi-yield
 * reboot — but is re-initialized on power-on, so a power cycle always re-runs
 * discovery. That's the point: a tier-3 board must re-evaluate and yield to a Pi
 * that is now present, rather than persist as a second hub. */
#define HUB_BOOT_MAGIC  0x48554221u   /* "HUB!" */
static RTC_DATA_ATTR uint32_t s_hub_boot_flag;

void role_boot_as_hub(void) {
    s_hub_boot_flag = HUB_BOOT_MAGIC;
    esp_restart();          /* never returns; the flag is read on the next boot */
}

bool role_pending_hub_boot(void) {
    if (s_hub_boot_flag == HUB_BOOT_MAGIC) {
        s_hub_boot_flag = 0;   /* consume once — a later failure reboots back into discovery */
        return true;
    }
    return false;
}

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) {   /* first boot / version bump → erase + retry */
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (role_pending_hub_boot()) {      /* this boot self-hubbed (roles.h) */
        ESP_LOGI(TAG, "role: hub+rover (self-hub, tier 3 home mode) — starting the on-chip hub");
        hub_role_run(true);     /* self-hub → drives + watches for a Pi to yield to; never returns */
    }

    rover_role_pref_t role = rover_config_load_role_pref();
    if (role == ROLE_HUB) {
        ESP_LOGI(TAG, "role: hub (role_pref, tier 2) — starting the on-chip hub");
        hub_role_run(false);    /* forced hub-only → never drives, never yields (would re-force, a loop) */
    }
    ESP_LOGI(TAG, "role: rover%s", role == ROLE_AUTO ? " (auto)" : " (role_pref)");
    rover_role_run();       /* never returns — self-hubs if no hub-* is found (AUTO) */
}
