/*
 * Wi-Fi + broker services of the unified image. TWO entry points:
 *
 *   board_run(self_broker_ok)  — the NORMAL board (tiers 1 + 3). Always APSTA:
 *       it raises its own open `rover-<id>` AP AND an STA uplink from line one,
 *       and never switches radio mode again. So home↔classroom is runtime state,
 *       not a boot role, and there is NO mode-switch reboot (the old RTC-flag
 *       self-hub claim is deleted — CLAUDE.md § Status & design history):
 *         - joins a hub-* (classroom) → drives off that shared broker (central).
 *         - no hub, AUTO → runs a LOCAL broker and drives itself (home/island).
 *         - no hub, ROVER-pinned → keeps looking (never self-brokers).
 *       Its AP is `rover-<id>` (NOT hub-*), so no other rover joins a home board.
 *
 *   hub_role_run()             — tier 2: a dedicated professor hub. Raises a
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
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"       /* IP4_ADDR — relocating the board AP off 192.168.4.0/24 */
#include "mosq_broker.h"
#include "mdns.h"
#include "roles.h"
#include "rover_config.h"        /* rover_config_load — the stored STA uplink */
#include "provisioning_util.h"   /* rover_format_robot_id — the board's AP SSID = its rover-id */
#include "wifi_portal.h"         /* the always-on :80 Wi-Fi config panel (rover.local/wifi) */

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

#ifndef PROFESSOR_PASS
#define PROFESSOR_PASS "change-me"     /* the one gated identity — see connect_cb */
#endif

#define DHCPS_OFFER_DNS 0x02

/* ws_mqtt_bridge.c — lets browsers reach the broker over MQTT-over-WebSocket */
void start_ws_mqtt_bridge(void);
void ws_bridge_reap_all(void);   /* force-close every open bridge — a departing
                                   * station's own bridge slot otherwise leaks */

static const char *TAG = "hub-broker";
static esp_netif_t *ap_netif;
static esp_netif_t *sta_netif;

/* STA state, shared with the event handler below. */
static volatile bool s_sta_got_ip = false;
static volatile bool s_want_connect = false;   /* gate auto-reconnect so it doesn't fight a scan */
static esp_ip4_addr_t s_gw;                    /* DHCP gateway — on a hub's AP, the hub/broker */

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

bool board_has_uplink(void)
{
    return s_sta_got_ip;
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
     * on `uplink === "full"`. "full" here means "the STA has an address"
     * (portal-vs-full needs an HTTP probe the Pi does — same honesty note as
     * /fleet). The portal page's own fields ride along; its JS reads `ssid`
     * for the network name since this change. */
    return snprintf(buf, len,
        "{\"state\":\"%s\",\"board\":\"%s\",\"role\":\"%s\",\"ssid\":\"%s\",\"uplink\":\"%s\",\"ip\":\"%s\",\"pin\":\"%s\",\"dash\":\"%s\"}",
        st_str[s_net_state], s_board_id, role, esc, s_sta_got_ip ? "full" : "none",
        ip, pesc, s_dash);
}

/* Session auth: whole-session accept/reject, the only gate this broker offers
 * (no per-topic ACL — CONTRACT.md § Discovery & isolation). The classroom's
 * real boundary is the hub's own Wi-Fi perimeter, not a login: every board
 * and browser is admitted with no credential at all — a name is a topic
 * address, not a password. The one gated identity is "professor" — not
 * because this port can enforce a narrower ACL for it (it can't), but so the
 * dashboard's fleet-wide controls (Stop-all, drive-any-robot) need a real
 * password before they light up: deliberate friction on the one set of
 * actions that shouldn't be a stray tap. Get "professor" right → admitted;
 * anything else, admitted too. (Confirmed 2026-07-13 — the per-robot
 * robot1/robot2/pool credential table this replaced never enforced anything
 * a determined student couldn't already read off a card; it just made every
 * fresh board a manual provisioning step.) */
static int connect_cb(const char *client_id, const char *username,
                      const char *password, int password_len)
{
    (void)password_len;
    const char *cid = client_id ? client_id : "(none)";
    if (username && strcmp(username, "professor") == 0) {
        if (password && strcmp(password, PROFESSOR_PASS) == 0) {
            ESP_LOGI(TAG, "accept %s as professor", cid);
            return 0;
        }
        ESP_LOGW(TAG, "reject %s: wrong professor password", cid);
        return 1;
    }
    ESP_LOGI(TAG, "accept %s%s%s", cid, username ? " as " : " (anonymous)", username ? username : "");
    return 0;
}

/* Hand AP clients a DNS server (the STA's), or they get an IP but can't resolve
 * names — the classic "connected, no internet". */
static void ap_offer_dns_from_sta(void)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return;
    }
    uint8_t offer = DHCPS_OFFER_DNS;
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer, sizeof(offer));
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(ap_netif);
}

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_got_ip = false;
        ESP_LOGW(TAG, "uplink down — retrying (AP + broker stay up regardless)");
        if (s_want_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the AP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "a device left the AP");
        ws_bridge_reap_all();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_gw = e->ip_info.gw;
        s_sta_got_ip = true;
        ESP_LOGI(TAG, "uplink up, got IP " IPSTR " — enabling NAT + DNS for AP clients",
                 IP2STR(&e->ip_info.ip));
        ap_offer_dns_from_sta();
        esp_netif_set_default_netif(sta_netif);
        if (esp_netif_napt_enable(ap_netif) != ESP_OK) {
            ESP_LOGE(TAG, "NAPT enable failed");
        } else {
            ESP_LOGI(TAG, "NAT on: AP clients now route to the internet via the uplink");
        }
    }
}

/* ── APSTA bring-up shared by both entry points ──────────────────────────────
 * Brings up netif + Wi-Fi in APSTA with the given open AP SSID and mDNS
 * hostname, and starts the radio. The STA leg is left unconfigured — the caller
 * sets it (a fixed uplink for the hub, a discovered/stored one for a board). */
static void wifi_apsta_up(const char *ap_ssid, const char *mdns_host, bool ap_alt_subnet)
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "APSTA up: AP '%s' (join this). AP channel follows the uplink's (single radio).",
             ap_ssid);

    /* mDNS: <host>.local — "hub" for a hub (matches the Pi's avahi name), "rover"
     * for a board, so a kid reaches their board's dashboard at http://rover.local/
     * (on the board's own AP it is the only responder, so the name is unambiguous;
     * a shared LAN with several boards would collide — the classroom uses the
     * dashboard's per-id list there, not rover.local). */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(mdns_host);
        mdns_instance_name_set("Better Robotics");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS up: dashboard also at http://%s.local/", mdns_host);
    } else {
        ESP_LOGW(TAG, "mDNS init failed (http://%s.local won't resolve; IP still works)", mdns_host);
    }
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
    esp_err_t e = esp_wifi_scan_start(NULL, true);
    s_want_connect = prev;                        /* restore: the STA may still be driving */
    if (e != ESP_OK) return 0;                    /* e.g. a discovery/hub-watch scan in flight */

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
    s_want_connect = true;
    esp_wifi_connect();
    ESP_LOGI(TAG, "uplink re-dial -> '%s' (live — AP and dashboard stay up)", ssid);
    return true;
}

/* ── hub-watch: an island yields to a real hub.
 * A board islanded because it saw no hub — but one may appear just after (a Pi
 * boots ~30-60 s slower than an ESP; a professor's hub is switched on; or our own
 * boot scan simply missed it). For a bounded window, watch for any `hub-*` and
 * step down to it. This is SAFE against peer islands because an island raises
 * `rover-<id>`, NOT `hub-*` — so a `hub-*` beacon can only be a *real* designated
 * hub (a Pi `hub-pi-*` or a tier-2 professor hub), never another home board.
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

/* ── board_run: the normal board, always APSTA (tiers 1 + 3) ──────────────────
 * self_broker_ok = AUTO (may become its own island if no hub is found); false =
 * ROVER-pinned (must never self-broker — keeps looking for a hub). Never returns;
 * never a mode-switch reboot — it re-evaluates in a loop instead. */
void board_run(bool self_broker_ok)
{
    rover_button_start();   /* recover button: hold to reboot (rover_role.c) */

    uint8_t stamac[6];
    esp_read_mac(stamac, ESP_MAC_WIFI_STA);
    char ap_ssid[16];
    rover_format_robot_id(stamac, ap_ssid);   /* "rover-<suffix>" — matches the board id */
    snprintf(s_board_id, sizeof s_board_id, "%s", ap_ssid);
    wifi_apsta_up(ap_ssid, "rover", true);    /* AP on 192.168.99.1 (so the STA can join a
                                               * hub cleanly); mDNS → rover.local */

    /* The Wi-Fi config panel — always on, on this board's :80, before any broker
     * decision. This is what makes "join rover.local, set your home Wi-Fi" work in
     * EVERY mode (a classroom rover has no broker/dashboard of its own otherwise).
     * When the board later islands, start_ws_mqtt_bridge registers the drive
     * dashboard onto this same :80 handle. */
    wifi_portal_start();

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

        char uri[80];
        if (joined_hub) {
            /* Classroom: the DHCP gateway is the hub/broker — central control. The
             * hub also serves the class dashboard on :80, and NAPT (armed on our
             * STA lease) lets a phone on THIS board's AP reach it — so the landing
             * page can hand out the URL. IP, not hub.local: mDNS is link-local and
             * doesn't cross the NAT hop. */
            snprintf(uri, sizeof uri, "mqtt://" IPSTR ":1883", IP2STR(&s_gw));
            char dash[32];
            snprintf(dash, sizeof dash, "http://" IPSTR "/", IP2STR(&s_gw));
            board_net_state_set(BOARD_NET_HUB, discovered, dash);
            ESP_LOGI(TAG, "joined hub '%s' — driving off its broker (central)", discovered);
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

        /* Session died — re-evaluate without a reboot. If we were islanding, the
         * broker + AP stay up and we simply re-dial localhost; if the classroom hub
         * vanished, the next pass rediscovers (and an AUTO board can now island). */
        ESP_LOGW(TAG, "drive session ended — re-evaluating (no reboot)");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ── hub_role_run: tier-2 dedicated professor hub ─────────────────────────────
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
    wifi_apsta_up(ap_ssid, "hub", false);     /* AP stays 192.168.4.1 — boards join THIS;
                                               * mDNS → hub.local (matches the Pi) */

    /* The config panel runs here too (hub.local/wifi): so designating a board as
     * HUB isn't a one-way trip (flip role back to auto), and the professor sets the
     * venue uplink below at runtime. Must start BEFORE start_ws_mqtt_bridge so the
     * dashboard registers onto this shared :80 instead of opening its own. */
    wifi_portal_start();

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
