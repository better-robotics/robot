/*
 * Wi-Fi + broker services of the unified image. TWO entry points:
 *
 *   board_run(self_broker_ok)  — the NORMAL board (tiers 1 + 3). Comes up APSTA:
 *       its own open `rover-<id>` AP AND an STA uplink, from line one. So
 *       home↔classroom is runtime state, not a boot role, and there is NO
 *       mode-switch reboot to *reach* a state (the old RTC-flag self-hub claim
 *       is deleted — CLAUDE.md § Status & design history):
 *         - joins a hub-* (classroom) → drives off that shared broker (central),
 *           and DROPS its AP for as long as it stays a hub client: one network
 *           in the room, the hub's (board_ap_down has the full argument).
 *         - no hub, AUTO → runs a LOCAL broker and drives itself (home/island).
 *         - no hub, ROVER-pinned → keeps looking (never self-brokers).
 *       Its AP is `rover-<id>` (NOT hub-*), so no other rover joins a home board.
 *       The only mode change is APSTA→STA on a hub join, which is safe live; the
 *       way BACK is a clean restart, never a live switch.
 *
 *   hub_role_run()             — tier 2: a dedicated instructor hub. Raises a
 *       `hub-*` AP (the SSID a rover's scan joins → it gathers a fleet), runs the
 *       broker, and does NOT drive (broker/AP vs real-time motors on one radio,
 *       hub#2). Chosen deliberately via role_pref=HUB.
 *
 * ESP32-as-hub is the full local slice on one chip: AP+STA+NAPT + Mosquitto +
 * per-robot connect-auth (feasibility validated on hardware 2026-07-09).
 *   - AP  (open)      : students/rovers/laptop join, no password.
 *   - STA (venue/home): uplink for internet (optional — the broker works offline).
 *   - NAPT            : forwards AP-side traffic out the STA leg, so joining the
 *                       AP does NOT cut internet.
 *   - broker :1883    : raw MQTT; browsers reach it via the :9001 WS bridge.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"           /* deferred STA redial — never vTaskDelay in the event task */
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"  /* esp_netif_get_netif_impl — the raw lwIP netif, for captive_nat */
#include "dhcpserver/dhcpserver.h" /* dhcps_offer_t / OFFER_DNS — the DHCP DNS offer flag */
#include "esp_mac.h"
#include "lwip/ip4_addr.h"       /* IP4_ADDR — relocating the board AP off 192.168.4.0/24 */
#include "lwip/sockets.h"        /* the uplink probe below speaks raw HTTP, like the Pi's */
#include "lwip/netdb.h"
#include "mosq_broker.h"
#include "mdns.h"
#include "roles.h"
#include "rover_config.h"        /* rover_config_load — the stored STA uplink */
#include "provisioning_util.h"   /* rover_format_robot_id — the board's AP SSID = its rover-id */
#include "device_log.h"
#include "ota_update.h"
#include "wifi_portal.h"         /* the always-on :80 Wi-Fi config panel (rover.local/wifi) */
#include "captive_nat.h"         /* packet-layer backstop for clients that bypass our DNS */

/* STA_SSID / STA_PASS — the tier-2 hub's venue uplink. Gitignored header so real
 * Wi-Fi credentials never land in committed source; copy wifi_creds.example.h. */
#include "wifi_creds.h"

#define AP_SSID_PREFIX "hub-"          /* tier-2 hub only (+ last 2 SoftAP MAC bytes
                                        * → hub-a3f2); a normal board uses its rover-id */
#define AP_PASS     ""                 /* open by default: rovers only auto-join OPEN
                                        * hub-*, and students join with no password. ""
                                        * → open; 8-63 chars → WPA2 (an optional per-board
                                        * password can ride with the #17 config panel). */
#define AP_CHANNEL  1                  /* overridden to match STA channel in APSTA (single radio) */
#define AP_MAX_CONN 8                  /* esp32_nat_router's documented ceiling */

#ifndef INSTRUCTOR_PASS
#define INSTRUCTOR_PASS "change-me"     /* the one gated identity — see connect_cb */
#endif

/* ws_mqtt_bridge.c — lets browsers reach the broker over MQTT-over-WebSocket */
void start_ws_mqtt_bridge(void);
void ws_bridge_reap_all(void);   /* force-close every open bridge — a departing
                                   * station's own bridge slot otherwise leaks */

static const char *TAG = "hub-broker";
static esp_netif_t *ap_netif;
static esp_netif_t *sta_netif;

/* Is our own AP still beaconing? A board drops it once cleanly joined to a hub
 * (board_ap_down); the tier-2 hub never does. Gates the two things that exist
 * only to serve AP clients: NAPT, and the rover.local alias. */
static volatile bool s_ap_up = false;

/* MDNS_BOARD_ALIAS — the friendly name, delegated, AP-only (wifi_apsta_up). The
 * board's PRIMARY mDNS name is its unique rover-<id>; see the mDNS block there. */
#define MDNS_BOARD_ALIAS "rover"

/* STA state, shared with the event handler below. */
static volatile bool s_sta_got_ip = false;
static volatile bool s_want_connect = false;   /* gate auto-reconnect so it doesn't fight a scan */
static esp_ip4_addr_t s_gw;                    /* DHCP gateway — on a hub's AP, the hub/broker */
static esp_timer_handle_t s_redial_timer;      /* deferred redial (backoff) — see wifi_events */
static volatile int s_redial_fails;            /* consecutive STA failures; 0 on got-IP / fresh join */
static volatile uint8_t s_last_disc_reason;    /* why the last STA attempt fell — feeds try_join's verdict */
static volatile bool s_portal_trial = false;   /* a portal trial-join owns the radio: hub-watch must not
                                                  scan (or worse, yield-restart) under it */

/* ── /wifi/status facts (roles.h board_net_state_t) ──────────────────────────
 * Owned here because every input is this file's: the broker decision (board_run),
 * the STA lease, the gateway. wifi_portal.c only serializes them out. */
static volatile board_net_state_t s_net_state = BOARD_NET_SEARCHING;
static char s_uplink_ssid[33];   /* the STA target we last committed to ("" = none) */
static char s_dash[64];          /* where the drive dashboard lives (roles.h) */
static char s_board_id[16];      /* rover-xxxx / hub-xxxx — set once at entry */

void board_net_state_set(board_net_state_t st, const char *uplink_ssid, const char *dash)
{
    s_net_state = st;
    snprintf(s_uplink_ssid, sizeof s_uplink_ssid, "%s", uplink_ssid ? uplink_ssid : "");
    snprintf(s_dash, sizeof s_dash, "%s", dash ? dash : "");
}

/* SSIDs are user/venue-supplied — escape " and \ , drop control bytes (same
 * rule as the scan list; the pages render them with textContent). */
static void json_esc_ssid(char *dst, size_t cap, const char *src)
{
    size_t e = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && e < cap - 2; p++) {
        if (*p < 0x20) continue;
        if (*p == '"' || *p == '\\') dst[e++] = '\\';
        dst[e++] = (char)*p;
    }
    dst[e] = 0;
}

/* The uplink SSID the STA last committed to, JSON-escaped ("" while
 * searching) — the sys beacon's "net" field, so the dashboard's per-card
 * network chip names the actual network instead of a bare IP. */
void board_uplink_ssid_json(char out[65])
{
    json_esc_ssid(out, 65, (const char *)s_uplink_ssid);
}

/* ── Uplink verdict (the Pi hubd's probe_uplink, ported) ─────────────────────
 * A DHCP lease is NOT internet: on a university visitor network the STA gets
 * an address but every HTTP request is intercepted by the venue's own captive
 * gate until THIS BOARD's MAC signs in (observed live 2026-07-14 — the
 * phone's captive sheet rendered the venue's SSO instead of /welcome, because
 * "got an IP" was being reported as "full"). So probe the way phones do:
 * fetch a known 204 endpoint over plain HTTP. 204 → FULL. Any other HTTP
 * answer → PORTAL — and its Location header IS the venue's sign-in URL,
 * captured so /welcome can hand it to the user. No answer → NONE. */
static volatile board_uplink_t s_uplink_verdict = BOARD_UPLINK_NONE;
static char s_portal_url[160];

board_uplink_t board_uplink(void)
{
    return s_sta_got_ip ? s_uplink_verdict : BOARD_UPLINK_NONE;
}

void board_portal_url(char out[160])
{
    snprintf(out, 160, "%s", s_portal_url);
}

bool board_has_uplink(void)
{
    return board_uplink() == BOARD_UPLINK_FULL;
}

static board_uplink_t probe_uplink_once(void)
{
    /* Same endpoint and verdict rules as hub/pi/src/bin/hubd.rs probe_uplink;
     * raw socket like the Pi (no esp_http_client dependency), plus Location
     * capture the Pi doesn't need (its venue sheet reaches the phone via
     * dnsmasq; ours is deliberately intercepted by dns_server.c's mixed mode,
     * so /welcome must carry the venue link itself). */
    static const char HOST[] = "connectivitycheck.gstatic.com";
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai = NULL;
    if (getaddrinfo(HOST, "80", &hints, &ai) != 0 || !ai) return BOARD_UPLINK_NONE;

    board_uplink_t verdict = BOARD_UPLINK_NONE;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s >= 0) {
        struct timeval tv = { .tv_sec = 8 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
            static const char req[] =
                "GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\n"
                "Connection: close\r\n\r\n";
            if (send(s, req, sizeof req - 1, 0) == (int)(sizeof req - 1)) {
                char buf[512];
                int n = recv(s, buf, sizeof buf - 1, 0);
                if (n > 12) {
                    buf[n] = 0;
                    /* "HTTP/1.1 204 ..." — the status word decides. */
                    char *sp = strchr(buf, ' ');
                    if (sp && strncmp(sp + 1, "204", 3) == 0) {
                        verdict = BOARD_UPLINK_FULL;
                    } else if (sp) {
                        verdict = BOARD_UPLINK_PORTAL;
                        char *loc = strstr(buf, "\r\nLocation: ");
                        if (!loc) loc = strstr(buf, "\r\nlocation: ");
                        if (loc) {
                            loc += 12;
                            char *end = strstr(loc, "\r\n");
                            size_t l = end ? (size_t)(end - loc) : strlen(loc);
                            if (l >= sizeof s_portal_url) l = sizeof s_portal_url - 1;
                            memcpy(s_portal_url, loc, l);
                            s_portal_url[l] = 0;
                        }
                    }
                }
            }
        }
        close(s);
    }
    freeaddrinfo(ai);
    if (verdict == BOARD_UPLINK_FULL) s_portal_url[0] = 0;
    return verdict;
}

static void uplink_probe_task(void *arg)
{
    (void)arg;
    /* The Pi's poll_uplink debounce, verbatim: recovery to FULL is instant;
     * downgrades need 3 agreeing probes (one flaky probe on a busy uplink
     * must not flash "no internet" at a classroom that has it). */
    board_uplink_t last = BOARD_UPLINK_NONE;
    int streak = 0;
    for (;;) {
        board_uplink_t v = s_sta_got_ip ? probe_uplink_once() : BOARD_UPLINK_NONE;
        streak = (v == last) ? streak + 1 : 1;
        last = v;
        if (v == BOARD_UPLINK_FULL || streak >= 3) {
            if (s_uplink_verdict != v) {
                static const char *name[] = { "none", "portal", "full" };
                ESP_LOGI(TAG, "uplink verdict: %s%s%s", name[v],
                         v == BOARD_UPLINK_PORTAL ? " — venue gate at " : "",
                         v == BOARD_UPLINK_PORTAL ? s_portal_url : "");
            }
            s_uplink_verdict = v;
        }
        /* Same cadence drives the captive presence reaper: forget the Accept of
         * any device that has left the AP, so its next join is greeted again. */
        captive_reap_absent();
        /* Faster while not-full: someone is likely mid-onboarding on /welcome. */
        vTaskDelay(pdMS_TO_TICKS(s_uplink_verdict == BOARD_UPLINK_FULL ? 15000 : 5000));
    }
}

int board_status_json(char *buf, size_t len)
{
    static const char *st_str[] = { "searching", "hub", "remote", "local" };
    char ip[16] = "";
    esp_netif_ip_info_t ipi;
    if (s_sta_got_ip && sta_netif && esp_netif_get_ip_info(sta_netif, &ipi) == ESP_OK)
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipi.ip));
    rover_role_pref_t rp = rover_config_load_role_pref();
    const char *role = rp == ROLE_HUB ? "hub" : rp == ROLE_ROVER ? "rover" : "auto";
    char esc[65], pin[33], pesc[65];
    json_esc_ssid(esc, sizeof esc, s_uplink_ssid);
    rover_config_load_hub_pin(pin);   /* surfaced so "did the pin apply" is visible */
    json_esc_ssid(pesc, sizeof pesc, pin);
    /* ssid + uplink follow the Pi hubd's /wifi/status dialect — the shared
     * dashboard shows "No uplink yet" unless `ssid` is set and gates health
     * on `uplink === "full"`. Since 2026-07-14 `uplink` carries the real
     * probe verdict (none/portal/full — board_uplink above), the same
     * vocabulary the Pi reports; `portal_url` is this firmware's own extra
     * (/welcome links the venue's sign-in gate through the NAT). */
    board_uplink_t up = board_uplink();
    char purl[160], puesc[200];
    board_portal_url(purl);
    json_esc_ssid(puesc, sizeof puesc, purl);
    return snprintf(buf, len,
        "{\"state\":\"%s\",\"board\":\"%s\",\"role\":\"%s\",\"ssid\":\"%s\",\"uplink\":\"%s\",\"ip\":\"%s\",\"pin\":\"%s\",\"dash\":\"%s\",\"portal_url\":\"%s\"}",
        st_str[s_net_state], s_board_id, role, esc,
        up == BOARD_UPLINK_FULL ? "full" : up == BOARD_UPLINK_PORTAL ? "portal" : "none",
        ip, pesc, s_dash, up == BOARD_UPLINK_PORTAL ? puesc : "");
}

/* Session auth: whole-session accept/reject, the only gate this broker offers
 * (no per-topic ACL — CONTRACT.md § Discovery & isolation). The classroom's
 * real boundary is the hub's own Wi-Fi perimeter, not a login: every board
 * and browser is admitted with no credential at all — a name is a topic
 * address, not a password. The one gated identity is "instructor" — not
 * because this port can enforce a narrower ACL for it (it can't), but so the
 * dashboard's fleet-wide controls (Stop-all, drive-any-robot) need a real
 * password before they light up: deliberate friction on the one set of
 * actions that shouldn't be a stray tap. Get "instructor" right → admitted;
 * anything else, admitted too. (Confirmed 2026-07-13 — the per-robot
 * robot1/robot2/pool credential table this replaced never enforced anything
 * a determined student couldn't already read off a card; it just made every
 * fresh board a manual provisioning step.) */
/* The instructor credential, in one place: NVS first, compile-time default
 * second. The literal is plaintext in the image and firmware.yml publishes
 * .bins from a PUBLIC repo, so a baked-in secret ships to whoever downloads
 * one; NVS keeps a real classroom's password off the shared image. Read
 * per-check, never cached: rotating it must not need a reboot.
 *
 * Shared with ota_update.c (POST /ota), which gates on the same identity over
 * HTTP Basic — one secret for both, so there is nothing to rotate twice. */
bool board_instructor_pass_ok(const char *given)
{
    if (!given) return false;
    char nvs_pass[65];
    rover_config_load_instructor_pass(nvs_pass);
    const char *want = nvs_pass[0] ? nvs_pass : INSTRUCTOR_PASS;
    return strcmp(given, want) == 0;
}

static int connect_cb(const char *client_id, const char *username,
                      const char *password, int password_len)
{
    (void)password_len;
    const char *cid = client_id ? client_id : "(none)";
    if (username && strcmp(username, "instructor") == 0) {
        if (board_instructor_pass_ok(password)) {
            ESP_LOGI(TAG, "accept %s as instructor", cid);
            return 0;
        }
        ESP_LOGW(TAG, "reject %s: wrong instructor password", cid);
        return 1;
    }
    ESP_LOGI(TAG, "accept %s%s%s", cid, username ? " as " : " (anonymous)", username ? username : "");
    return 0;
}

/* AP clients always resolve through THIS board — we hand our own IP out as the
 * DHCP DNS offer EXPLICITLY (wifi_apsta_up, below), the Pi's dnsmasq "polite
 * fast path": a well-behaved client then asks us directly and dns_server.c
 * decides per query whether to hijack or forward upstream, and captive_nat.c
 * only has to catch the clients that pin their own resolver. This used to lean
 * on "the dhcps default offers our IP as DNS" — an unverified assumption behind
 * a load-bearing behavior, made explicit 2026-07-19. The old design switched
 * the offer to the venue's DNS on got-IP — removed 2026-07-14: a DHCP offer
 * can't be un-offered (leases outlive state changes), and on a captive venue it
 * handed phones a resolver that routed their probes straight into the venue's
 * SSO page instead of /welcome. One policy point (the forwarder) beats two. */
/* Fires from the esp_timer task — esp_wifi_connect is thread-safe, and the gate
 * re-check means a redial armed before a scan/re-dial cleared it is a no-op. */
static void redial_cb(void *arg)
{
    (void)arg;
    if (s_want_connect) esp_wifi_connect();
}

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_got_ip = false;
        s_last_disc_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_uplink_verdict = BOARD_UPLINK_NONE;   /* a lost lease is a hard fact — no debounce */
        if (s_want_connect) {
            /* Redial with backoff, not immediately: every attempt at an absent
             * uplink is a full off-channel scan (~2 s) that mutes the AP, and an
             * immediate redial per disconnect looped that scan continuously — a
             * hub with a dead venue uplink stuttered every rover and phone on its
             * AP for as long as the outage lasted (single radio). First retries
             * stay immediate: a rebooting hub is the COMMON disconnect and comes
             * back in seconds (robot#1, the 2026-07-10 AP-bounce test). The 30 s
             * ceiling caps the AP's scan loss at ~5% while still catching a venue
             * uplink that comes up late. */
            int fails = ++s_redial_fails;
            if (fails <= 3) {
                ESP_LOGW(TAG, "uplink down — retrying (AP + broker stay up regardless)");
                esp_wifi_connect();
            } else {
                int sh = fails - 4;
                uint32_t delay_ms = sh >= 3 ? 30000 : (5000u << sh);   /* 5s,10s,20s → 30s */
                ESP_LOGW(TAG, "uplink down — retry in %lus (AP + broker stay up regardless)",
                         (unsigned long)(delay_ms / 1000));
                esp_timer_stop(s_redial_timer);   /* re-arm cleanly; a no-op if idle */
                esp_timer_start_once(s_redial_timer, (uint64_t)delay_ms * 1000);
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the AP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "a device left the AP");
        ws_bridge_reap_all();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_gw = e->ip_info.gw;
        s_sta_got_ip = true;
        s_redial_fails = 0;   /* connected — the next outage starts at fast retries again */
        ESP_LOGI(TAG, "uplink up, got IP " IPSTR " — enabling NAT (probe decides full vs portal)",
                 IP2STR(&e->ip_info.ip));
        esp_netif_set_default_netif(sta_netif);
        /* NAPT exists to route OUR AP's clients out the uplink. A hub-joined
         * board has no AP (board_ap_down), so arming it would be inert at best
         * — and this handler re-fires on every STA reconnect, so without the
         * gate a re-dial to the hub would quietly re-arm the very door
         * board_ap_down closed. */
        if (!s_ap_up) {
            ESP_LOGI(TAG, "NAT stays off — no AP of our own to route (hub client)");
        } else if (esp_netif_napt_enable(ap_netif) != ESP_OK) {
            ESP_LOGE(TAG, "NAPT enable failed");
        } else {
            ESP_LOGI(TAG, "NAT on: AP clients now route out the uplink");
        }
    }
}

/* ── APSTA bring-up shared by both entry points ──────────────────────────────
 * Brings up netif + Wi-Fi in APSTA with the given open AP SSID and mDNS
 * hostname, and starts the radio. The STA leg is left unconfigured — the caller
 * sets it (a fixed uplink for the hub, a discovered/stored one for a board).
 * ap_alias = an extra, friendly <alias>.local answered ONLY on our own AP
 * (NULL for none) — see the mDNS block below. */
static void wifi_apsta_up(const char *ap_ssid, const char *mdns_host, const char *ap_alias,
                          bool ap_alt_subnet)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    /* A board keeps its OWN AP up while its STA joins a hub — but every ESP32
     * softAP defaults to 192.168.4.1/24, the SAME subnet the hub leases from, so
     * the STA can associate yet never route (two interfaces, one subnet → DHCP/
     * routing breaks; observed: board associates with hub-e349, gets no IP, and
     * wrongly falls back to islanding). Relocate the board's AP to 192.168.99.0/24
     * so its STA can pull a clean 192.168.4.x lease. The dedicated hub keeps
     * .4.1 (it's the one boards join, not the other way round). */
    /* DHCP option 114 (RFC 8910/8908 — "how to modernize your captive
     * network", confirmed 2026-07-14): advertises the captive-portal URI
     * directly in the DHCP offer instead of making the OS guess via probe
     * redirects. iOS 14+/macOS Big Sur+/Android 11+ read it; Windows doesn't.
     * Apple's full spec wants the companion JSON status API served over TLS
     * — impractical for a private AP with no real certificate — so this is
     * the DHCP-hint half only: a strict client may ignore a plain-HTTP URI
     * and fall back to the probe flow this file already answers regardless.
     * CONFIRMED ignored by macOS 2026-07-19: serial showed the CNA reach for
     * /hotspot-detect.html, never /captive-portal-api. Kept for Android 11+,
     * which does read it; do not expect it to be the trigger on Apple.
     * esp_netif_dhcps_option stores the POINTER, not a copy (dhcpserver.c),
     * so the buffer must outlive the DHCP server — static, not stack-local. */
    static char s_captive_uri[48];
    snprintf(s_captive_uri, sizeof s_captive_uri, "http://%s/captive-portal-api",
             ap_alt_subnet ? "192.168.99.1" : "192.168.4.1");
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    if (ap_alt_subnet) {
        esp_netif_ip_info_t ip = {0};
        IP4_ADDR(&ip.ip, 192, 168, 99, 1);
        IP4_ADDR(&ip.gw, 192, 168, 99, 1);
        IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip));
    }
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           s_captive_uri, strlen(s_captive_uri) + 1));
    /* Offer OUR OWN IP as the client's DNS resolver, explicitly (see the comment
     * on the redial section for why this stopped being an assumption). Read the
     * AP's actual IP back rather than hardcode it — it's just been set to .99.1
     * or is the default .4.1. dns_server.c answers :53 for whatever asks. */
    esp_netif_ip_info_t ap_ip = {0};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ap_ip));
    esp_netif_dns_info_t ap_dns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
    ap_dns.ip.u_addr.ip4.addr = ap_ip.ip.addr;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns));
    dhcps_offer_t dns_offer = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &dns_offer, sizeof dns_offer));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_events, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_events, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {
        .ap = {
            .channel = AP_CHANNEL,
            .password = AP_PASS, .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    size_t ap_ssid_len = strlen(ap_ssid);
    memcpy(ap.ap.ssid, ap_ssid, ap_ssid_len);
    ap.ap.ssid_len = ap_ssid_len;
    if (strlen(AP_PASS) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    const esp_timer_create_args_t redial_args = { .callback = redial_cb, .name = "sta-redial" };
    ESP_ERROR_CHECK(esp_timer_create(&redial_args, &s_redial_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_up = true;   /* before any STA join can fire IP_EVENT_STA_GOT_IP, which reads it */
    /* Default WIFI_PS_MIN_MODEM dozes the radio between DTIM beacons — but drive
     * commands reach a classroom rover over this STA leg, so modem sleep puts
     * beacon-interval latency spikes on the joystick path (and delays AP-side
     * service). The ~50 mA it saves is noise next to the motors. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* Pin the AP to HT20 rather than the driver's HT20/40 auto: in a classroom
     * packed with other 2.4 GHz APs a clean 40 MHz secondary channel rarely
     * exists, so HT40 there only adds interference and negotiation for throughput
     * this workload never uses (tiny control/telemetry plus a rate-capped camera
     * fit HT20 with airtime to spare) — and HT20 is the more robust link. The
     * STA/uplink leg is left at auto so an ESP-hub's internet isn't capped. */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_LOGI(TAG, "APSTA up: AP '%s' (join this). AP channel follows the uplink's (single radio).",
             ap_ssid);

    /* Backstop for clients that bypass our DNS entirely — a hardcoded resolver,
     * DoH, or a cached probe-host IP (dns_server.c's hijack only works for a
     * client that actually asks US, and a laptop that has been online asks
     * nobody). See captive_nat.c's banner: it steers every un-greeted AP
     * client's :53/:80 to this board in EVERY uplink state (the Pi's rule),
     * releasing a device the moment it taps Continue — so installing it
     * unconditionally is correct, not merely safe. */
    captive_nat_install((struct netif *)esp_netif_get_netif_impl(ap_netif));

    /* ── mDNS: two names, split by which link they belong to ─────────────────
     * PRIMARY (mdns_hostname_set) = the UNIQUE name: "hub" for a hub (matches
     * the Pi's avahi name), "rover-<id>" for a board. The responder tracks its
     * addresses automatically on every netif, so it is right on the board's own
     * AP and on a hub's LAN alike, with no maintenance.
     *
     * Why unique-as-primary, when a bare "rover" reads nicer: the primary is
     * the name that gets MANGLED on a collision. RFC 6762 conflict resolution
     * is implemented here (mdns_receive.c, mangle_name → "-2", "-3"...), so N
     * boards sharing a LAN do NOT fail — they silently become rover, rover-2,
     * rover-3..., and `rover.local` resolves to WHICHEVER BOOTED FIRST. A name
     * that works and points somewhere arbitrary is worse than one that doesn't
     * resolve, and the ordinals land in the same namespace as the MAC-suffix
     * ids (rover-2 next to rover-3f2a, indistinguishable by shape). MAC
     * suffixes don't collide, so a unique primary never mints one.
     *
     * ALIAS (delegated) = the friendly "rover.local", pinned to our AP's own
     * IP. A delegated hostname carries a STATIC address list the caller must
     * maintain — normally the catch, and here exactly why it fits: this name
     * is only ever answered on our own AP, whose IP is fixed above, so the
     * list is a constant. Read back from the netif rather than restating
     * 192.168.99.1 — one source for the AP's address. That pinning IS the
     * policy: rover.local is the AP's name, rover-<id>.local is the board's,
     * so "stop claiming rover.local on a hub" needs no separate rule — it
     * falls out of dropping the AP (board_ap_down). */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(mdns_host);
        mdns_instance_name_set("Better Robotics");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS up: dashboard also at http://%s.local/", mdns_host);
        esp_netif_ip_info_t ap_ip = {0};
        if (ap_alias && esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK) {
            mdns_ip_addr_t a = { .addr = { .type = ESP_IPADDR_TYPE_V4 }, .next = NULL };
            a.addr.u_addr.ip4.addr = ap_ip.ip.addr;
            if (mdns_delegate_hostname_add(ap_alias, &a) == ESP_OK) {
                ESP_LOGI(TAG, "mDNS alias: http://%s.local/ (on our AP only)", ap_alias);
            } else {
                ESP_LOGW(TAG, "mDNS alias '%s' failed (http://%s.local still works)",
                         ap_alias, mdns_host);
            }
        }
    } else {
        ESP_LOGW(TAG, "mDNS init failed (http://%s.local won't resolve; IP still works)", mdns_host);
    }

    /* One probe task for the boot, both entry points — the uplink verdict
     * (none/portal/full) that /wifi/status, dns_server.c, and the captive
     * Accept flip all read. */
    xTaskCreate(uplink_probe_task, "uplink-probe", 4096, NULL, 4, NULL);
}

/* Set the STA config and (re)connect, waiting up to 30 s for an IP. In APSTA a
 * failed/aborted STA leg leaves the AP untouched. Returns true once we have an IP. */
static bool sta_join(const char *ssid, const char *pass)
{
    s_sta_got_ip = false;
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Never ESP_ERROR_CHECK here: if the auto-reconnect handler left an attempt
     * in flight (dead AP), set_config returns ESP_ERR_WIFI_STATE — abort()ing on
     * it crash-rebooted every rover whose hub vanished (robot#1, caught in the
     * 2026-07-10 AP-bounce test). Abort the attempt and retry once instead. */
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (e == ESP_ERR_WIFI_STATE) {
        s_want_connect = false;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "sta_join '%s': set_config failed (%s)", ssid, esp_err_to_name(e));
        return false;   /* caller's loop re-evaluates; the AP stays up regardless */
    }
    s_redial_fails = 0;   /* a fresh deliberate join earns the fast retries again */
    s_want_connect = true;
    esp_wifi_connect();
    for (int i = 0; i < 120 && !s_sta_got_ip; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_sta_got_ip) { s_want_connect = false; esp_wifi_disconnect(); }
    return s_sta_got_ip;
}

/* Zero-touch onboarding: an OPEN network named hub-* is the classroom
 * convention, so its existence is all the config a rover needs; strongest wins.
 * A hub PIN (NVS, rover_config_set_hub_pin) narrows admission to one exact SSID
 * (rover_hub_admits) — the rogue-hub guard. */
/* One active scan; returns the strongest admissible hub found (or false). */
static bool scan_for_hub(char out[33], const char *pin)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    wifi_ap_record_t *ap = malloc(n * sizeof *ap);   /* heap, not stack: ~80 B each */
    if (!ap) return false;
    esp_wifi_scan_get_ap_records(&n, ap);
    int best = -1;
    for (int i = 0; i < n; i++)
        if (ap[i].authmode == WIFI_AUTH_OPEN &&
            rover_hub_admits((const char *)ap[i].ssid, pin) &&
            (best < 0 || ap[i].rssi > ap[best].rssi))
            best = i;
    if (best >= 0) {
        snprintf(out, 33, "%s", (const char *)ap[best].ssid);
        ESP_LOGI(TAG, "found %s (%d dBm)", out, ap[best].rssi);
    }
    free(ap);
    return best >= 0;
}

/* Zero-touch onboarding: an OPEN network named hub-* is the classroom convention,
 * so its existence is all the config a rover needs; strongest wins. Retried a few
 * times — a single active scan (especially while our own AP is beaconing) can miss
 * an AP that IS there, and a false "no hub" wrongly drops an AUTO board to islanding. */
#define HUB_SCAN_TRIES 3
static bool discover_hub(char out[33])
{
    /* Quiesce the STA leg first. Clearing the gate only stops FUTURE reconnects;
     * an attempt already in flight (the handler redialing a vanished AP) keeps
     * the driver in "connecting", where every scan is refused ("scan not
     * allowed") — so a rover whose hub died could never SEE the hub coming back
     * (robot#1, 2026-07-10 AP-bounce test). Disconnect aborts the attempt; on an
     * idle STA it's a harmless no-op. */
    s_want_connect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));   /* let the driver settle before scanning */
    char pin[33];
    rover_config_load_hub_pin(pin);
    if (pin[0]) ESP_LOGI(TAG, "scanning for pinned hub '%s' (foreign hub-* ignored)", pin);
    else        ESP_LOGI(TAG, "scanning for an open hub-* network");
    for (int t = 0; t < HUB_SCAN_TRIES; t++) {
        if (scan_for_hub(out, pin)) return true;
    }
    return false;
}

/* Scan for the Wi-Fi config panel (roles.h): every visible network, deduped by
 * SSID (strongest kept), so a student can pick their home network by name. Shares
 * the radio with drive/discovery, so it borrows the same s_want_connect discipline
 * as discover_hub — but RESTORES the gate afterward (a panel scan is incidental to
 * a live session, unlike discovery which is about to re-issue its own connect). */
int board_wifi_scan(board_ap_t *out, int max)
{
    bool prev = s_want_connect;
    s_want_connect = false;                       /* don't let a reconnect fire mid-scan */
    /* Wait out a colliding scan instead of reporting an empty room. Only one
     * scan can run on the radio, and hub_watch_task starts one every
     * HUB_WATCH_SCAN_MS — so a student's tap lands inside a ~2 s scan roughly
     * one time in ten. That refusal used to `return 0`, which is the SAME value
     * as "scanned fine, saw nothing": the picker rendered "No networks found."
     * at someone standing in an apartment block. Found on the wire 2026-07-17
     * — first tap [], second tap the real list.
     *
     * Three tries at 1.2 s covers the longest in-flight scan; a caller that
     * still can't have the radio gets -1 (busy), never 0, so "we couldn't look"
     * and "there is nothing there" stay different facts all the way up to the
     * page. Blocking here is not new — esp_wifi_scan_start(NULL, true) already
     * blocks this httpd task for the scan's duration. */
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        e = esp_wifi_scan_start(NULL, true);
        if (e == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    s_want_connect = prev;                        /* restore: the STA may still be driving */
    if (e != ESP_OK) return -1;                   /* radio busy — NOT "no networks" */

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    wifi_ap_record_t *ap = malloc(n * sizeof *ap);   /* heap: ~80 B each — never on the stack */
    if (!ap) return 0;
    esp_wifi_scan_get_ap_records(&n, ap);

    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *ss = (const char *)ap[i].ssid;
        if (!ss[0]) continue;                     /* hidden network — no name to show */
        int at = -1;
        for (int j = 0; j < count; j++)
            if (strcmp(out[j].ssid, ss) == 0) { at = j; break; }
        if (at >= 0) {                            /* same SSID seen already — keep the stronger */
            if (ap[i].rssi > out[at].rssi) out[at].rssi = ap[i].rssi;
            continue;
        }
        snprintf(out[count].ssid, sizeof out[count].ssid, "%s", ss);
        out[count].rssi = ap[i].rssi;
        out[count].open = (ap[i].authmode == WIFI_AUTH_OPEN);
        count++;
    }
    free(ap);
    return count;
}

/* Set by hub_role_run: the dedicated hub applies new uplink credentials with
 * a live re-dial (its STA is fire-and-forget + event-handler reconnect);
 * board/rover mode keeps the config-apply reboot — its loop owns the radio
 * mid-session. */
static volatile bool s_hub_role = false;

bool board_wifi_redial(const char *ssid, const char *pass)
{
    if (!s_hub_role) return false;
    /* Gate OFF first so the disconnect event can't race an auto-reconnect
     * against the OLD credentials while we swap the config in. */
    s_want_connect = false;
    s_sta_got_ip = false;
    esp_wifi_disconnect();
    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, ssid, strnlen(ssid, sizeof sta.sta.ssid));
    memcpy(sta.sta.password, pass, strnlen(pass, sizeof sta.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) {
        s_want_connect = true;   /* leave the reconnect gate as it was */
        return false;
    }
    board_net_state_set(BOARD_NET_LOCAL, ssid, "/");
    s_redial_fails = 0;   /* new credentials earn the fast retries again */
    s_want_connect = true;
    esp_wifi_connect();
    ESP_LOGI(TAG, "uplink re-dial -> '%s' (live — AP and dashboard stay up)", ssid);
    return true;
}

/* Trial-join for the portal (board/rover mode, where the apply is still a
 * config-apply reboot): attempt the credentials on the STA leg and block
 * until an IP lands or ~20 s passes. In APSTA the AP — and the portal page
 * on it — stays up throughout, which is the whole point: a wrong password
 * becomes feedback in place instead of a reboot into a dead uplink with the
 * bad credentials saved. Returns NULL on success; otherwise a short human
 * verdict, after re-dialing whatever uplink was configured before
 * (best-effort — a fresh island has nothing to restore). s_portal_trial
 * parks hub-watch for the duration: its blocking scan would mute the join,
 * and a mid-trial yield-restart would cut this page off exactly the way the
 * trial exists to prevent. */
const char *board_wifi_try_join(const char *ssid, const char *pass)
{
    static char verdict[64];   /* one portal caller at a time — httpd serializes */
    s_portal_trial = true;
    s_want_connect = false;    /* gate OFF before touching config, same as redial */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_config_t old = {0};
    esp_wifi_get_config(WIFI_IF_STA, &old);

    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, ssid, strnlen(ssid, sizeof sta.sta.ssid));
    memcpy(sta.sta.password, pass, strnlen(pass, sizeof sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) {
        s_portal_trial = false;
        return "the radio is busy — try again";
    }
    s_sta_got_ip = false;
    s_last_disc_reason = 0;
    s_redial_fails = 0;
    s_want_connect = true;     /* the event handler's fast retries ride along */
    esp_wifi_connect();
    for (int i = 0; i < 80 && !s_sta_got_ip; i++) vTaskDelay(pdMS_TO_TICKS(250));

    if (s_sta_got_ip) {
        s_portal_trial = false;
        ESP_LOGI(TAG, "trial join '%s' verified (IP acquired) — caller may now persist + reboot", ssid);
        return NULL;
    }

    /* Verdict from the last disconnect reason, then put the old uplink back. */
    s_want_connect = false;
    esp_wifi_disconnect();
    switch (s_last_disc_reason) {
    case WIFI_REASON_NO_AP_FOUND:
        snprintf(verdict, sizeof verdict, "network not found — is it in range?"); break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        snprintf(verdict, sizeof verdict, "wrong password?"); break;
    case 0:
        snprintf(verdict, sizeof verdict, "joined but got no address from it"); break;
    default:
        snprintf(verdict, sizeof verdict, "couldn't join (reason %u)", s_last_disc_reason);
    }
    ESP_LOGW(TAG, "trial join '%s' failed: %s", ssid, verdict);
    if (old.sta.ssid[0]) {
        if (esp_wifi_set_config(WIFI_IF_STA, &old) == ESP_OK) {
            s_want_connect = true;
            esp_wifi_connect();   /* fire-and-forget; the event handler owns retries */
        }
    }
    s_portal_trial = false;
    return verdict;
}

/* ── hub-watch: an island yields to a real hub.
 * A board islanded because it saw no hub — but one may appear just after (a Pi
 * boots ~30-60 s slower than an ESP; a instructor's hub is switched on; or our own
 * boot scan simply missed it). For a bounded window, watch for any `hub-*` and
 * step down to it. This is SAFE against peer islands because an island raises
 * `rover-<id>`, NOT `hub-*` — so a `hub-*` beacon can only be a *real* designated
 * hub (a Pi `hub-pi-*` or a tier-2 instructor hub), never another home board.
 * Yielding = a clean esp_restart (NOT a mode switch, no RTC flag): board_run
 * re-runs, discovers the now-present hub, and joins it as a rover.
 *
 * The watch is PERPETUAL, not a bounded window: a teacher who powers the hub
 * after the kids' boards would otherwise get a dead room until every board is
 * hand-rebooted (the 3-min window was too short for real classroom boot order,
 * 2026-07-10). The cost of a forever-scan — a brief AP+STA interruption every
 * scan — is avoided by SKIPPING the scan whenever the board was driven in the
 * last few seconds: an idle island keeps looking, an actively-driven one never
 * hiccups. (Home solo use is the only case that scans forever with no payoff;
 * idle-gated, that's an invisible ~100 ms blip every 20 s with nobody driving.) */
#define HUB_WATCH_SCAN_MS    20000   /* slow — an active scan interrupts AP+STA briefly */
#define HUB_WATCH_DRIVE_QUIET_MS 5000 /* skip the scan if driven within this window */
#define HUB_SCAN_MAX_AP 20           /* heap-sized cap; a classroom sees far fewer hubs */

static bool sees_hub_to_yield_to(const uint8_t self_bssid[6])
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    /* Heap, NOT stack: wifi_ap_record_t is ~80 B on IDF 5.5, so [20] is ~1.6 KB —
     * a stack array here overran hub-watch's task stack and panicked the board into
     * a reboot loop (Stack protection fault, observed on the C3 2026-07-09). */
    wifi_ap_record_t *ap = malloc(HUB_SCAN_MAX_AP * sizeof *ap);
    if (!ap) return false;
    uint16_t n = HUB_SCAN_MAX_AP;
    bool yield = false;
    char pin[33];
    rover_config_load_hub_pin(pin);   /* a pinned island yields ONLY to its own hub */
    if (esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            const char *ss = (const char *)ap[i].ssid;
            if (memcmp(ap[i].bssid, self_bssid, 6) == 0) continue;   /* our own AP beacon */
            if (ap[i].authmode == WIFI_AUTH_OPEN && rover_hub_admits(ss, pin)) {
                ESP_LOGW(TAG, "yield: hub '%s' present — stepping down (a real hub is preferred)", ss);
                yield = true;
                break;
            }
        }
    }
    free(ap);
    return yield;
}

static void hub_watch_task(void *arg)
{
    (void)arg;
    uint8_t self_bssid[6];
    esp_read_mac(self_bssid, ESP_MAC_WIFI_SOFTAP);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HUB_WATCH_SCAN_MS));
        if (rover_ms_since_drive() < HUB_WATCH_DRIVE_QUIET_MS)
            continue;    /* being driven — don't hiccup the link with a scan */
        if (s_portal_trial)
            continue;    /* a portal trial-join owns the radio — and a yield-
                            restart now would cut that page off mid-verdict */
        if (sees_hub_to_yield_to(self_bssid)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();   /* clean restart → board_run → discovery → join the hub */
        }
    }
}

static void broker_task(void *arg)
{
    (void)arg;
    struct mosq_broker_config bcfg = {
        .host = "0.0.0.0", .port = 1883, .tls_cfg = NULL, .handle_connect_cb = connect_cb,
    };
    ESP_LOGI(TAG, "starting broker on 0.0.0.0:1883 (raw MQTT; browsers via the :9001 WS bridge)");
    mosq_broker_run(&bcfg);   /* blocks for the life of the broker */
    ESP_LOGE(TAG, "broker exited");
    vTaskDelete(NULL);
}

/* ── board_ap_down: a hub client has no AP of its own ─────────────────────────
 * Once cleanly joined to a hub, a board's own AP has no job left: dashboard,
 * broker and config all live on the hub, reachable over the STA leg. Keeping it
 * up cost three things, in ascending order of what actually matters:
 *   1. Beacons on the HUB'S OWN CHANNEL. One radio means the AP cannot pick a
 *      channel — it follows the STA. So every board's beacons contend with the
 *      exact link carrying its own drive commands, and PS is deliberately NONE.
 *   2. A second, password-less door into the classroom network, via NAPT: join
 *      any rover-<id> and you route straight to the hub's broker without ever
 *      touching the hub's Wi-Fi. (Not a privilege escalation — the hub's AP is
 *      open too — but it makes "connect to the hub" a suggestion, not a fact.)
 *   3. A second topology to explain. The room should be "everything is on the
 *      hub", not "everything is on the hub, and also each robot is its own
 *      network". CLAUDE.md asks for topology that is explicit, never emergent;
 *      a board that is both a hub client and its own AP is the third state that
 *      principle was written against.
 * hub#3 named this exact mitigation ("drop the beacon when cleanly joined to a
 * hub") and gated it on measuring the beacon cost. Reason 3 doesn't need the
 * measurement, so the gate is spent on it.
 *
 * APSTA→STA is safe to do live: it is subtractive, and the STA keeps its channel
 * and its association. The REVERSE is not — a live STA→APSTA switch is the exact
 * thing always-APSTA exists to dodge, and it cost two hardware bugs (the
 * RTC_DATA wipe loop, the pi-watch stack panic; CLAUDE.md § Status & design
 * history). So coming back is a clean restart, never a mode flip — see board_run. */
static void board_ap_down(void)
{
    if (!s_ap_up) return;
    /* rover.local is the AP's name, pinned to the AP's IP (wifi_apsta_up) — with
     * no AP there is no link for it to answer on, and on a hub LAN it is the
     * name that would collide with every peer. The unique rover-<id>.local
     * (primary) is untouched and now answers over the hub's LAN. */
    mdns_delegate_hostname_remove(MDNS_BOARD_ALIAS);
    esp_netif_napt_disable(ap_netif);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        /* Not fatal: a board that keeps beaconing still drives fine — it just
         * costs the airtime above. Don't panic a classroom over it. */
        ESP_LOGW(TAG, "AP down failed (%s) — still beaconing, drive path unaffected",
                 esp_err_to_name(err));
        return;
    }
    s_ap_up = false;
    ESP_LOGI(TAG, "AP down — one network in the room: the hub's. This board is now "
                  "http://%s.local/ on the hub's LAN.", s_board_id);
}

/* ── board_run: the normal board (tiers 1 + 3) ────────────────────────────────
 * self_broker_ok = AUTO (may become its own island if no hub is found); false =
 * ROVER-pinned (must never self-broker — keeps looking for a hub). Never returns.
 * Comes up APSTA always; drops to STA-only for as long as it is a hub client
 * (board_ap_down), and restarts rather than switch back live. */
void board_run(bool self_broker_ok)
{
    rover_button_start();   /* recover button: hold to reboot (rover_role.c) */

    uint8_t stamac[6];
    esp_read_mac(stamac, ESP_MAC_WIFI_STA);
    char ap_ssid[16];
    rover_format_robot_id(stamac, ap_ssid);   /* "rover-<suffix>" — matches the board id */
    snprintf(s_board_id, sizeof s_board_id, "%s", ap_ssid);
    /* AP on 192.168.99.1 (so the STA can join a hub cleanly). mDNS: primary is the
     * UNIQUE rover-<id>.local (survives a hub LAN full of peers); "rover.local" is
     * the friendly alias, answered on this AP only. */
    wifi_apsta_up(ap_ssid, ap_ssid, MDNS_BOARD_ALIAS, true);

    /* The Wi-Fi config panel — always on, on this board's :80, before any broker
     * decision, so it is already serving whatever this board turns out to be. It
     * is what makes "join rover.local, set your home Wi-Fi" work on an ISLAND —
     * the only case that needs it, since a board with no hub has no dashboard
     * until it self-brokers, and its uplink can't be set over a network it hasn't
     * joined. A hub-joined board drops its AP below and reaches this panel only
     * over the hub's LAN (rover-<id>.local), which is enough: its name and pins
     * arrive over MQTT. When the board islands, start_ws_mqtt_bridge registers the
     * drive dashboard onto this same :80 handle. */
    wifi_portal_start();

    /* Firmware push (POST /ota) onto that same handle, and the self-test that
     * confirms a pushed image — so a board reached over the hub's LAN at
     * rover-<id>.local, or over its own AP when islanded, can be updated
     * without a cable. Both roles call this; neither special-cases the other. */
    ota_update_start();
    device_log_serve();   /* GET /log on the same handle — see device_log.h */

    /* Camera (esp32cam only; a no-op elsewhere). After Wi-Fi so it fits in what
     * memory is left, and after the portal so :80 is already claimed — the camera
     * takes :81. Init failure is non-fatal; sys advertises the stream only if it's
     * actually up (camera_running). */
    camera_start();

    bool broker_started = false;
    for (;;) {
        char ssid[33], pass[65], loc[65];
        rover_config_load(ssid, pass, loc);

        char discovered[33] = "";
        bool joined = false, joined_hub = false;
        board_net_state_set(BOARD_NET_SEARCHING, "", "");

        /* 1. a hub in range wins — the classroom IS the venue. Stored-first here
         * broke the yield promise: a board with home Wi-Fi stored would join it,
         * island, and hub_watch would then reboot it in a loop (restart → stored
         * joins again → island → watch fires again) for as long as a hub-* was
         * visible, never once trying the hub. Cost of hub-first at home: one
         * (retried) scan of empty air per boot before the stored join. */
        if (discover_hub(discovered) && sta_join(discovered, "")) {
            joined = joined_hub = true;
        }
        /* 2. else the stored uplink (home Wi-Fi, or an explicit choice). */
        if (!joined && ssid[0]) {
            ESP_LOGI(TAG, "trying stored network '%s'", ssid);
            joined = sta_join(ssid, pass);
            if (!joined) ESP_LOGW(TAG, "stored network '%s' unreachable", ssid);
        }

        /* Coming back from hub-client to anything else needs our AP again, and a
         * live STA→APSTA switch is precisely what always-APSTA exists to avoid.
         * Restart instead — board_run re-runs and is APSTA from line one. This is
         * hub_watch_task's own yield idiom pointed the other way, and the cost
         * lands only where the hub already vanished, i.e. nothing was driving. */
        if (!joined_hub && !s_ap_up) {
            ESP_LOGW(TAG, "no hub and our AP is down — restarting to come back up APSTA");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        char uri[80];
        if (joined_hub) {
            /* Classroom: the DHCP gateway is the hub/broker — central control. The
             * hub also serves the class dashboard on :80. IP, not hub.local: mDNS
             * is link-local, and this board is a plain station on the hub's LAN. */
            snprintf(uri, sizeof uri, "mqtt://" IPSTR ":1883", IP2STR(&s_gw));
            char dash[32];
            snprintf(dash, sizeof dash, "http://" IPSTR "/", IP2STR(&s_gw));
            board_net_state_set(BOARD_NET_HUB, discovered, dash);
            ESP_LOGI(TAG, "joined hub '%s' — driving off its broker (central)", discovered);
            /* The room has ONE network now: the hub's. Everything this AP used to
             * offer (config panel, captive onboarding, NAT to the hub dashboard)
             * is either on the hub already or is an island-only concern. */
            board_ap_down();
        } else if (joined && loc[0] && strncmp(loc, "mqtt://", 7) == 0) {
            /* A stored network with an explicit broker locator (e.g. a home Pi). */
            snprintf(uri, sizeof uri, "%s", loc);
            char host[48], dash[64];
            size_t h = strcspn(loc + 7, ":/");   /* host part of mqtt://host[:port] */
            snprintf(host, sizeof host, "%.*s", (int)(h < sizeof host ? h : sizeof host - 1), loc + 7);
            snprintf(dash, sizeof dash, "http://%s/", host);
            board_net_state_set(BOARD_NET_REMOTE, ssid, dash);
            ESP_LOGI(TAG, "joined '%s' — driving off stored broker %s", ssid, loc);
        } else if (self_broker_ok) {
            /* Home/island: no hub broker reachable — be our own. The uplink (if any)
             * just gives internet; drive comes from the on-chip broker. */
            ESP_LOGW(TAG, "no hub reachable — self-hubbing (home mode, island)");
            if (!broker_started) {
                xTaskCreate(broker_task, "broker", 4096, NULL, 5, NULL);
                start_ws_mqtt_bridge();
                xTaskCreate(hub_watch_task, "hub-watch", 4096, NULL, 4, NULL);
                broker_started = true;
                vTaskDelay(pdMS_TO_TICKS(2000));   /* let the broker bind :1883 first */
            }
            snprintf(uri, sizeof uri, "mqtt://127.0.0.1:1883");
            /* Only now does dash say "/" — the bridge above owns :80's / from here,
             * so a landing page that reloads on "/" always gets the dashboard. */
            board_net_state_set(BOARD_NET_LOCAL, joined ? ssid : "", "/");
        } else {
            /* ROVER-pinned with no hub in range: never self-broker. Keep the AP up
             * (reconfigurable) and rescan shortly — no reboot. */
            ESP_LOGW(TAG, "no hub reachable and role is ROVER — rescanning shortly");
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        rover_client_run(uri);   /* blocks driving the board; returns on a dead session */

        /* Session died — re-evaluate. If we were islanding, the broker + AP stay up
         * and we simply re-dial localhost (no reboot). If the classroom hub
         * vanished, the next pass rediscovers: still there → rejoin in place; gone
         * → the AP has to come back, so the check at the top of the loop restarts. */
        ESP_LOGW(TAG, "drive session ended — re-evaluating");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ── hub_role_run: tier-2 dedicated instructor hub ─────────────────────────────
 * role_pref=HUB. Raises a hub-* AP a rover's scan joins, runs the broker, does
 * NOT drive. Never returns (blocks in the broker). */
void hub_role_run(void)
{
    s_hub_role = true;   /* enables the live /wifi/connect re-dial path */
    uint8_t apmac[6];
    esp_read_mac(apmac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[16];
    snprintf(ap_ssid, sizeof ap_ssid, AP_SSID_PREFIX "%02x%02x", apmac[4], apmac[5]);
    snprintf(s_board_id, sizeof s_board_id, "%s", ap_ssid);
    wifi_apsta_up(ap_ssid, "hub", NULL, false);   /* AP stays 192.168.4.1 — boards join THIS;
                                                   * mDNS → hub.local (matches the Pi). No alias:
                                                   * hub.local IS the unique name, and a hub
                                                   * never drops its AP. */

    /* The config panel runs here too (hub.local/wifi): so designating a board as
     * HUB isn't a one-way trip (flip role back to auto), and the instructor sets the
     * venue uplink below at runtime. Must start BEFORE start_ws_mqtt_bridge so the
     * dashboard registers onto this shared :80 instead of opening its own. */
    wifi_portal_start();

    /* Firmware push (POST /ota) + the pushed-image self-test, on that same
     * handle — see board_run's call. A dedicated hub is the board least likely
     * to be within reach of a cable, so it needs this most. */
    ota_update_start();
    device_log_serve();   /* GET /log on the same handle — see device_log.h */

    /* Venue uplink: a stored network (set from the panel) wins; else the gitignored
     * compile-time creds. Fire-and-forget — the broker must come up NOW (the
     * classroom works offline), so we don't block on the join; the event handler
     * reconnects and NAT arms if it lands. */
    char ssid[33], pass[65], loc[65];
    rover_config_load(ssid, pass, loc);
    wifi_config_t sta = {0};   /* zero-init: unused tail stays NUL, no terminator needed */
    const char *up_ssid = ssid[0] ? ssid : STA_SSID;
    const char *up_pass = ssid[0] ? pass : STA_PASS;
    size_t sl = strnlen(up_ssid, sizeof sta.sta.ssid);
    size_t pl = strnlen(up_pass, sizeof sta.sta.password);
    memcpy(sta.sta.ssid, up_ssid, sl);
    memcpy(sta.sta.password, up_pass, pl);
    ESP_LOGI(TAG, "hub uplink: %s '%s'", ssid[0] ? "stored" : "compile-time", up_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    s_want_connect = true;
    esp_wifi_connect();

    start_ws_mqtt_bridge();
    board_net_state_set(BOARD_NET_LOCAL, up_ssid, "/");   /* dashboard is here */
    struct mosq_broker_config bcfg = {
        .host = "0.0.0.0", .port = 1883, .tls_cfg = NULL, .handle_connect_cb = connect_cb,
    };
    ESP_LOGI(TAG, "starting broker on 0.0.0.0:1883 (raw MQTT; browsers via the :9001 WS bridge)");
    mosq_broker_run(&bcfg);   /* blocks */
}
