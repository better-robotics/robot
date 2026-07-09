#ifndef ROLES_H
#define ROLES_H

/* The two boot roles of the unified image (hub self-election, DESIGN-unified.md).
 * app_main (main.c) reads role_pref from NVS, picks one, and calls it. Each role
 * owns its own radio setup — rover = Wi-Fi STA client, hub = AP+STA+NAPT — and
 * never returns (both block forever), so only one runs per boot and the two
 * radio configs never coexist at runtime. Shared init (NVS) runs in main.c. */
void rover_role_run(void);   /* esp-mqtt client + L298N motor drive (rover_role.c) */
void hub_role_run(void);     /* on-chip broker + WS bridge + NAT (hub_role.c) */

#endif
