#ifndef ROVER_CONFIG_H
#define ROVER_CONFIG_H
#include <stdbool.h>
#include "esp_err.h"

/* Board identity rides telemetry as metadata — the robot id stays role-named
 * (rover-XXXX), hardware never enters the name. */
#ifndef HW_BOARD
#define HW_BOARD "esp32"   /* each PlatformIO env passes its own */
#endif

/* Unset fields load as "". NVS holds only explicit choices — a rover needs
 * nothing stored: no ssid → scan-join an open hub-*, no locator → the
 * network gateway. */
void rover_config_load(char ssid[33], char pass[65], char locator[65]);
esp_err_t rover_config_set_wifi(const char *ssid, const char *pass);
esp_err_t rover_config_set_locator(const char *locator);
bool rover_config_is_complete(void);   /* = ssid set; everything else is derivable */

/* MQTT identity, assigned post-join over robots/<id>/cmd/config and persisted.
 * Unset → the compile-time MQTT_USER/PASS defaults; name is a human label (""
 * if unset). This is how a rover picks up its team from the hub dashboard
 * instead of a compile-time flag or BLE onboarding. */
void rover_config_load_identity(char user[33], char pass[65], char name[33]);
esp_err_t rover_config_set_identity(const char *user, const char *pass, const char *name);

/* Motor drive pins, assigned post-join like the team — a student wires their own
 * chassis, so the pinout can't be a compile-time constant. Order is fixed:
 * {ena, in1, in2, enb, in3, in4}. load leaves `pins` untouched when nothing is
 * stored, so the caller seeds it with the compile-time defaults first (absent =
 * keep defaults). set REJECTS any pin outside 0..33 (ESP32 output-capable) so a
 * bad value can never persist and boot-loop the board. */
bool rover_config_load_motor_pins(int pins[6]);
esp_err_t rover_config_set_motor_pins(const int pins[6]);

/* Boot role for the unified image. The dispatcher (main.c) reads this to pick
 * board_run (AUTO/ROVER, always-APSTA) vs hub_role_run (HUB, tier-2 professor
 * hub). AUTO is the default: the board may become its own island if no hub is
 * found; ROVER pins it to never self-broker; HUB makes it a dedicated hub. Set
 * from the hub dashboard / config page. Stored as one byte under "role"; load
 * returns AUTO when unset or unrecognized. The numeric values are persisted, so
 * keep them stable. */
typedef enum { ROLE_AUTO = 0, ROLE_HUB = 1, ROLE_ROVER = 2 } rover_role_pref_t;
rover_role_pref_t rover_config_load_role_pref(void);
esp_err_t rover_config_set_role_pref(rover_role_pref_t role);

#endif
