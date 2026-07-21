#ifndef ROBOT_PROVISIONING_UTIL_H
#define ROBOT_PROVISIONING_UTIL_H
#include <stdbool.h>
#include <stdint.h>
bool robot_validate_locator(const char *s);
void robot_format_robot_id(const uint8_t mac[6], char out[16]);

/* Hub admission, shared by discovery and hub-watch (hub_role.c): an open
 * network is a joinable hub iff it's named hub-* AND, when a pin is set, its
 * SSID equals the pin exactly — a pinned board never joins a foreign hub.
 * The pin is TOFU state set post-join (cmd/config {"hub":"hub-a045"} → NVS,
 * cleared with "hub":""). pin NULL/"" = unpinned (any open hub-*). */
bool robot_hub_admits(const char *ssid, const char *pin);

/* Is `host` one of OUR OWN origins — an IP literal, an mDNS *.local, or a bare
 * single-label name — rather than a public DNS name? One discriminator, two
 * callers: the captive 404 (a public Host is somebody's probe → 302 /welcome;
 * ours keep an honest 404) and the config-POST guard (a public Host on a
 * state-changing POST is a DNS-rebinding pivot through our wildcard responder →
 * refuse it: only our own origins may reconfigure the board). Empty/NULL counts
 * as ours — a captive probe always carries a Host, a rebind carries a real name. */
bool robot_host_is_local(const char *host);
#endif
