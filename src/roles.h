#ifndef ROLES_H
#define ROLES_H

#include <stdbool.h>

/* The two boot roles of the unified image (hub self-election, DESIGN-unified.md).
 * app_main (main.c) reads role_pref from NVS, picks one, and calls it. Each role
 * owns its own radio setup — rover = Wi-Fi STA client, hub = AP+STA+NAPT — and
 * never returns (both block forever), so only one runs per boot and the two
 * radio configs never coexist at runtime. Shared init (NVS) runs in main.c. */
void rover_role_run(void);      /* esp-mqtt client + L298N motor drive (rover_role.c) */
void hub_role_run(bool elected);/* on-chip broker + WS bridge + NAT (hub_role.c). `elected`
                                 * = won the election (vs forced role_pref=HUB): only an
                                 * elected hub abdicates — a forced one rebooting would just
                                 * be re-forced into the hub role, an infinite loop. */

/* Hub self-election (DESIGN-unified.md § Hub election). The claim is transient:
 * a rover that wins the election requests a hub boot via an RTC-memory flag and
 * reboots — the dispatcher then runs the hub role with a clean radio init (no
 * STA→APSTA re-init in one boot). RTC memory survives esp_restart but resets on
 * power-on, so a power cycle always re-elects — an ESP hub yields to a Pi that
 * appears later. hub_role's abdication reboots WITHOUT the flag to step down. */
void role_boot_as_hub(void);       /* set the flag + esp_restart into the hub role (never returns) */
bool role_pending_hub_boot(void);  /* dispatcher: true once, if this boot was an election claim */

/* SSID classification shared by the election (rover) and abdication (hub).
 * Any open "hub-*" is a hub to join/yield to; "hub-pi-*" additionally marks the
 * Pi, which ESP hubs ALWAYS yield to regardless of MAC (DESIGN-unified.md
 * mitigation c — the Pi is the preferred hub). */
#define HUB_SSID_PREFIX     "hub-"
#define HUB_PI_SSID_PREFIX  "hub-pi-"

#endif
