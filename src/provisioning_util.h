#ifndef ROVER_PROVISIONING_UTIL_H
#define ROVER_PROVISIONING_UTIL_H
#include <stdbool.h>
#include <stdint.h>
bool rover_validate_locator(const char *s);
void rover_format_robot_id(const uint8_t mac[6], char out[16]);

/* Hub admission, shared by discovery and hub-watch (hub_role.c): an open
 * network is a joinable hub iff it's named hub-* AND, when a pin is set, its
 * SSID equals the pin exactly — a pinned board never joins a foreign hub.
 * The pin is TOFU state set post-join (cmd/config {"hub":"hub-a045"} → NVS,
 * cleared with "hub":""). pin NULL/"" = unpinned (any open hub-*). */
bool rover_hub_admits(const char *ssid, const char *pin);
#endif
