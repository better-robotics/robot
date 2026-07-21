#ifndef ROBOT_CONFIG_H
#define ROBOT_CONFIG_H
#include <stdbool.h>
#include "esp_err.h"

/* Board identity rides telemetry as metadata — the robot id stays role-named
 * (robot-XXXX), hardware never enters the name. */
#ifndef HW_BOARD
#define HW_BOARD "esp32"   /* each PlatformIO env passes its own */
#endif

/* Unset fields load as "". NVS holds only explicit choices — a robot needs
 * nothing stored: no ssid → scan-join an open hub-*, no locator → the
 * network gateway. */
void robot_config_load(char ssid[33], char pass[65], char locator[65]);
esp_err_t robot_config_set_wifi(const char *ssid, const char *pass);
/* "Forget this network" (dashboard's Set-up-Wi-Fi panel, POST /wifi/forget):
 * erase the stored uplink only — name, role, motor pins, and hub pin survive,
 * since those are identity/hardware facts, not the venue/home network. */
esp_err_t robot_config_clear_wifi(void);

/* Identity, assigned post-join over robots/<id>/cmd/config and persisted.
 * Unset → the compile-time ROBOT_NAME default ("unassigned"). This is how a
 * robot picks up its name from the hub dashboard instead of a compile-time
 * flag or BLE onboarding. No password: the name is a topic address, not a
 * credential — the hub's own Wi-Fi is the classroom's real boundary
 * (CONTRACT.md § Discovery & isolation). */
void robot_config_load_identity(char name[33]);
esp_err_t robot_config_set_identity(const char *name);
/* Reprovision: erase the stored identity — the board boots back into the
 * unassigned pool. Wi-Fi, motor pins and the hub pin survive: venue and
 * hardware facts, not identity. */
esp_err_t robot_config_clear_identity(void);

/* Motor drive pins, assigned post-join like the name — a student wires their own
 * chassis, so the pinout can't be a compile-time constant. Order is fixed:
 * {ena, in1, in2, enb, in3, in4}. load leaves `pins` untouched when nothing is
 * stored, so the caller seeds it with the compile-time defaults first (absent =
 * keep defaults). set REJECTS any pin outside 0..33 (ESP32 output-capable) so a
 * bad value can never persist and boot-loop the board. */
bool robot_config_load_motor_pins(int pins[6]);
esp_err_t robot_config_set_motor_pins(const int pins[6]);

/* Hub pin (optional, rogue-hub guard): when set, discovery and hub-watch admit
 * ONLY this exact hub SSID (robot_hub_admits) — a pinned board never joins a
 * foreign hub-*, so a student raising their own hub-XXXX can't absorb it.
 * Trust-on-first-use: assigned post-join via cmd/config {"hub":"hub-a045"},
 * cleared with {"hub":""}. Unset/"" = any open hub-*. set REJECTS a value that
 * isn't "" or hub-* (≤32 chars) so a typo can't silently strand the board. */
void robot_config_load_hub_pin(char pin[33]);
esp_err_t robot_config_set_hub_pin(const char *pin);

/* Operator password for the hub role's connect_cb (fleet/estop is the one
 * thing the open ACL can't hand out for free). Stored in NVS, NOT compiled in:
 * OPERATOR_PASS is a plaintext literal in the image, `robot` is a public repo,
 * and firmware.yml uploads flashable .bins — so a build-time secret ships to
 * anyone who downloads one, and `strings firmware.bin` reads it straight out.
 * NVS keeps it off the shared image and per-board, changeable without a
 * reflash, exactly like the Wi-Fi credential above. Unset → the compile-time
 * OPERATOR_PASS default, so an un-provisioned board behaves as before.
 * Only a board someone designates HUB ever consults this: one step, once, not
 * per-robot provisioning (the cost that killed the old credential table). */
void robot_config_load_operator_pass(char pass[65]);
esp_err_t robot_config_set_operator_pass(const char *pass);

/* Boot role for the unified image. The dispatcher (main.c) reads this to pick
 * board_run (AUTO/ROBOT, APSTA at boot) vs hub_role_run (HUB, tier-2 operator
 * hub). AUTO is the default: the board may become its own island if no hub is
 * found; ROBOT pins it to never self-broker; HUB makes it a dedicated hub. Set
 * from the hub dashboard / config page. Stored as one byte under "role"; load
 * returns AUTO when unset or unrecognized. The numeric values are persisted, so
 * keep them stable. */
typedef enum { ROLE_AUTO = 0, ROLE_HUB = 1, ROLE_ROBOT = 2 } robot_role_pref_t;
robot_role_pref_t robot_config_load_role_pref(void);
esp_err_t robot_config_set_role_pref(robot_role_pref_t role);

#endif
