#pragma once

/* captive_nat — packet-layer backstop for the captive-portal probe hijack.
 *
 * dns_server.c only catches probe hostnames if the client actually asks
 * THIS board's DNS (the AP's DHCP-handed-out resolver). A client with its
 * own hardcoded/DoH resolver (observed: a Mac with Wi-Fi DNS pinned to
 * 8.8.8.8) resolves captive.apple.com to Apple's real address and sails
 * straight past the hijack — its probe either hits real internet (killing
 * the whole point: no dashboard popup) or hangs against an unreachable
 * address (looks like "no internet", not "captive").
 *
 * This installs a hand-rolled NAT pair on the AP netif's raw lwIP struct:
 * on receive, an unacked AP client's UDP/TCP traffic to port 53/80 gets its
 * destination address rewritten to the board's own AP IP (so it lands on
 * dns_server.c / the portal httpd exactly like an already-hijacked query),
 * recorded in a small flow table; on transmit, a reply from our own :53/:80
 * gets its SOURCE address rewritten back to whatever the client originally
 * queried, using that table — otherwise the client's own TCP/IP stack
 * discards a reply from an address it never contacted. lwIP's NAPT
 * (ip4_napt.c, vendored in this SDK) does the same address-rewrite-plus-
 * checksum-fixup trick for the opposite case (outbound masquerade); there is
 * no ready-made API for this direction (ip_portmap_add forwards INTO the
 * board from outside, not out-then-back).
 */

struct netif;

/* Call once, right after the AP netif's Wi-Fi driver is started
 * (wifi_apsta_up, after esp_wifi_start) — the raw netif must exist and its
 * input/linkoutput pointers must already be the driver's real ones, since
 * this saves and wraps them. Installing twice is a no-op (idempotent). */
void captive_nat_install(struct netif *ap_netif_impl);
