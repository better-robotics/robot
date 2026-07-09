#ifndef ROLES_H
#define ROLES_H

#include <stdbool.h>

/* The role tiers of the unified image (DESIGN-unified.md § Direction change).
 * app_main (main.c) reads role_pref from NVS, picks one, and calls it. Each role
 * owns its own radio setup — rover = Wi-Fi STA client, hub = AP+STA+NAPT — and
 * never returns (both block forever), so only one runs per boot and the two
 * radio configs never coexist at runtime. Shared init (NVS) runs in main.c. */
void rover_role_run(void);       /* tier 1: STA client → hub, L298N drive (rover_role.c) */
void hub_role_run(bool self_hub);/* on-chip broker + WS bridge + NAT (hub_role.c). `self_hub`
                                  * = this board self-hubbed with no Pi/hub present (tier 3,
                                  * home mode): it ALSO drives (a local rover client) and
                                  * watches for a Pi to yield to. false = a forced role_pref=
                                  * HUB (tier 2, professor hub): hub-only, no drive, no yield
                                  * — a forced hub rebooting would just be re-forced (a loop). */

/* rover_client_run — the MQTT client + motor-drive loop with NO Wi-Fi setup of
 * its own (rover_role.c). Shared by tier 1 (rover, after joining a hub, broker =
 * gateway) and tier 3 (the self-hub board driving itself, broker = localhost).
 * Assumes networking is already up; returns on a dead session, never reboots. */
void rover_client_run(const char *broker_uri);

/* Self-hub claim-by-reboot (DESIGN-unified.md § boot state machine). Transient:
 * an AUTO board that finds no hub-* sets an RTC-memory flag and reboots — the
 * dispatcher then runs the hub role (tier 3) with a clean radio init, not a
 * STA→APSTA re-init in one boot. RTC memory survives esp_restart but resets on
 * power-on, so a power cycle re-evaluates — a tier-3 board yields to a Pi that
 * appears later. hub_role's Pi-watch reboots WITHOUT the flag to step down. */
void role_boot_as_hub(void);       /* set the flag + esp_restart into the hub role (never returns) */
bool role_pending_hub_boot(void);  /* dispatcher: true once, if this boot was a self-hub claim */

/* SSID classification shared by discovery (rover) and Pi-watch (hub). Any open
 * "hub-*" is a hub to join; "hub-pi-*" additionally marks the Pi, which a tier-3
 * self-hub board ALWAYS yields to (DESIGN-unified.md § Pi-preference — the Pi is
 * the preferred hub; peer ESP hubs are left alone). */
#define HUB_SSID_PREFIX     "hub-"
#define HUB_PI_SSID_PREFIX  "hub-pi-"

#endif
