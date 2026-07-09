/*
 * HUB ROLE of the unified rover image (hub self-election, DESIGN-unified.md).
 * Moved here from better-robotics/hub/esp32 2026-07-09 when the ESP hub folded
 * into this firmware — a rover that finds no hub-* becomes one. `app_main` is now
 * the dispatcher in main.c; this file's entry is hub_role_run(), which the
 * dispatcher calls when role_pref = hub. Shared init (NVS) already ran there.
 *
 * ESP32-as-hub — the full local-hub slice on one plain ESP32:
 * AP+STA+NAPT + Mosquitto broker + per-team connect-auth. (Feasibility
 * validated on hardware 2026-07-09; the exploration issue is closed.)
 *   - AP  (open)            : students/rovers/laptop join here, no password.
 *                             SSID depends on role — a tier-2 hub raises
 *                             `hub-<suffix>` (the `hub-*` SSID a rover's scan
 *                             joins, so it gathers a fleet); a tier-3 self-hub
 *                             raises `rover-<id>` (deliberately NOT hub-*, so no
 *                             other rover joins a home board — each is an island).
 *   - STA (venue Wi-Fi)     : uplink for internet.
 *   - NAPT                  : forwards AP-side traffic out the STA leg, so
 *                             joining the AP does NOT cut internet (the thing
 *                             that stranded the laptop in the AP-only test).
 *   - broker :1883          : starts unconditionally — the classroom works
 *                             locally even with no uplink; internet layers on
 *                             if/when the STA gets an IP.
 *
 * Browsers reach the broker via the WS bridge (ws_mqtt_bridge.c); rover/sim/
 * mosquitto_pub speak raw TCP directly.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mosq_broker.h"
#include "mdns.h"
#include "roles.h"
#include "provisioning_util.h"   /* rover_format_robot_id — tier-3 AP SSID = the rover-id */

/* STA_SSID / STA_PASS — the venue uplink. Kept in a gitignored header so real
 * Wi-Fi credentials never land in committed source; copy wifi_creds.example.h
 * to wifi_creds.h and fill it in. */
#include "wifi_creds.h"

/* --- AP: what students/rovers/laptop join --- */
#define AP_SSID_PREFIX "hub-"          /* tier-2 hub only (+ last 2 SoftAP MAC bytes
                                        * → hub-a3f2); tier-3 self-hub uses the rover-id */
#define AP_PASS     ""                 /* open by default: rovers only auto-join OPEN
                                        * hub-*, and students join with no password. ""
                                        * → open; 8-63 chars → WPA2 (an optional per-board
                                        * password can ride with the #17 config panel). */
#define AP_CHANNEL  1                  /* overridden to match STA channel in APSTA (single radio) */
#define AP_MAX_CONN 8                  /* esp32_nat_router's documented ceiling */

#define DHCPS_OFFER_DNS 0x02

/* ws_mqtt_bridge.c — lets browsers reach the broker over MQTT-over-WebSocket */
void start_ws_mqtt_bridge(void);

static const char *TAG = "hub-broker";
static esp_netif_t *ap_netif;
static esp_netif_t *sta_netif;

/* Session auth: whole-session accept/reject, the only gate this broker port
 * offers (no per-topic ACL). On the ESP32, isolation is therefore connect-auth
 * + rover convention — each rover subscribes only its own robots/<id> — while
 * the Pi enforces the same robots/<id> ownership per-topic (CONTRACT.md §
 * Discovery & isolation). Credentials mirror classroom.example.json5: a team's
 * rover shares its TEAM identity (robot-id == team), so there is no standalone
 * rover credential. */
static int connect_cb(const char *client_id, const char *username,
                      const char *password, int password_len)
{
    static const struct { const char *u, *p; } creds[] = {
        { "professor", "change-me" },
        { "team1",     "change-me-team1" },
        { "team2",     "change-me-team2" },
    };
    const char *cid = client_id ? client_id : "(none)";
    /* DEMO-ONLY: allow anonymous (no username) so the dashboard's credential-
     * free public fleet view works against this single broker. Per-topic
     * enforcement of the read-only tier isn't possible here (no ACL); the Pi
     * hub gives anonymous read-only robots/# for the same view. */
    if (!username) {
        ESP_LOGI(TAG, "accept %s (anonymous, demo read tier)", cid);
        return 0;
    }
    if (!password) {
        ESP_LOGW(TAG, "reject %s: username with no password", cid);
        return 1;
    }
    for (size_t i = 0; i < sizeof(creds) / sizeof(creds[0]); i++) {
        if (strcmp(username, creds[i].u) == 0 && strcmp(password, creds[i].p) == 0) {
            ESP_LOGI(TAG, "accept %s as '%s'", cid, username);
            return 0;
        }
    }
    ESP_LOGW(TAG, "reject %s: bad credentials for '%s'", cid, username);
    return 1;
}

/* Hand AP clients a DNS server (the STA's), or they get an IP but can't
 * resolve names — the classic "connected, no internet". */
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
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "uplink down — retrying (AP + broker stay up regardless)");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the AP");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "uplink up, got IP " IPSTR " — enabling NAT + DNS for AP clients",
                 IP2STR(&e->ip_info.ip));
        ap_offer_dns_from_sta();
        esp_netif_set_default_netif(sta_netif);
        if (esp_netif_napt_enable(ap_netif) != ESP_OK) {
            ESP_LOGE(TAG, "NAPT enable failed");
        } else {
            ESP_LOGI(TAG, "NAT on: AP clients now route to the internet via the venue uplink");
        }
    }
}

/* ── Pi-preference (DESIGN-unified.md § Pi-preference) ────────────────────────
 * A tier-3 board self-hubbed because it saw no hub — but a Pi boots ~30-60 s
 * slower than an ESP, so it may appear just after. For a bounded window, watch
 * for the Pi (`hub-pi-*`) ONLY and step down to it — peer ESP hubs are left
 * alone (islands are acceptable and self-heal on reboot; the old lowest-MAC
 * election tiebreak is deleted). Yielding = esp_restart WITHOUT the RTC flag: the
 * dispatcher re-runs discovery and we join the Pi as a rover. Each active scan
 * briefly blips the single-radio AP, so the cadence is slow and the window
 * closes once the slow-Pi race is past — a home board (no Pi ever) then just
 * commits and stops scanning. */
#define PI_WATCH_WINDOW_MS  180000   /* watch the first ~3 min, then commit as the hub */
#define PI_WATCH_SCAN_MS     20000   /* slow — an active scan interrupts AP+STA briefly */

static bool sees_pi_to_yield_to(const uint8_t self_bssid[6]) {
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    wifi_ap_record_t ap[32];
    uint16_t n = sizeof ap / sizeof ap[0];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) return false;
    for (int i = 0; i < n; i++) {
        const char *ss = (const char *)ap[i].ssid;
        if (memcmp(ap[i].bssid, self_bssid, 6) == 0) continue;   /* our own AP beacon */
        if (strncmp(ss, HUB_PI_SSID_PREFIX, sizeof HUB_PI_SSID_PREFIX - 1) == 0) {
            ESP_LOGW(TAG, "yield: Pi hub '%s' present — stepping down (Pi is preferred)", ss);
            return true;
        }
    }
    return false;
}

static void pi_watch_task(void *arg) {
    (void)arg;
    uint8_t self_bssid[6];
    esp_read_mac(self_bssid, ESP_MAC_WIFI_SOFTAP);
    for (uint32_t waited = 0; waited < PI_WATCH_WINDOW_MS; waited += PI_WATCH_SCAN_MS) {
        vTaskDelay(pdMS_TO_TICKS(PI_WATCH_SCAN_MS));
        if (sees_pi_to_yield_to(self_bssid)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();   /* → dispatcher → discovery → join the Pi as a rover */
        }
    }
    ESP_LOGI(TAG, "no Pi appeared — committed as the hub (Pi-watch window closed)");
    vTaskDelete(NULL);
}

/* Tier 3 (home mode): the self-hub board ALSO drives itself. Once the local
 * broker is up (below), a rover client dials it over the loopback — the same
 * MQTT drive path a classroom rover runs, so the home dashboard drives exactly
 * like the classroom one. One-shot: a dead loopback session is a real bug, not
 * something to mask by rebooting the hub out from under its own broker. */
static void local_rover_task(void *arg) {
    (void)arg;
    /* Yield first: this task (prio 4) preempts app_main (prio 1) the instant it's
     * created, which is BEFORE app_main reaches mosq_broker_run below and binds
     * :1883. Delaying hands the CPU back so the broker is listening before we
     * dial the loopback — otherwise the first connect hits esp-mqtt's ~10 s
     * reconnect backoff and overruns rover_client_run's connect window. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    rover_client_run("mqtt://127.0.0.1:1883");   /* blocks driving; returns only if the session dies */
    ESP_LOGE(TAG, "local rover client exited — home-mode drive is down (hub stays up)");
    vTaskDelete(NULL);
}

void hub_role_run(bool self_hub)  /* dispatched from main.c; never returns (blocks in the broker) */
{
    /* NVS is already initialized by the dispatcher (main.c). */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_events, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_events, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Tier-2 hub → hub-<last 2 SoftAP MAC bytes> (e.g. hub-a3f2): the shared
     * `hub-*` SSID a rover's scan joins (CONTRACT.md). Tier-3 self-hub → the
     * rover-id (from the STA MAC, e.g. rover-a044): matches the board id the
     * dashboard shows, and is deliberately NOT a hub-* SSID, so no other rover's
     * discovery latches onto a home board — each self-hub board is its own island. */
    char ap_ssid[16];
    int ap_ssid_len;
    if (self_hub) {
        uint8_t stamac[6];
        esp_read_mac(stamac, ESP_MAC_WIFI_STA);
        rover_format_robot_id(stamac, ap_ssid);     /* "rover-<suffix>" */
        ap_ssid_len = strlen(ap_ssid);
    } else {
        uint8_t apmac[6];
        esp_read_mac(apmac, ESP_MAC_WIFI_SOFTAP);
        ap_ssid_len = snprintf(ap_ssid, sizeof ap_ssid,
                               AP_SSID_PREFIX "%02x%02x", apmac[4], apmac[5]);
    }

    wifi_config_t ap = {
        .ap = {
            .channel = AP_CHANNEL,
            .password = AP_PASS, .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(ap.ap.ssid, ap_ssid, ap_ssid_len);
    ap.ap.ssid_len = ap_ssid_len;
    if (strlen(AP_PASS) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config_t sta = {
        .sta = { .ssid = STA_SSID, .password = STA_PASS },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "APSTA up: AP '%s' (join this), STA → '%s' (uplink). "
                  "AP channel follows the venue's (single radio).", ap_ssid, STA_SSID);

    /* mDNS: advertise hostname "hub" so Apple/Bonjour clients reach the
     * dashboard at http://hub.local/ (matches the Pi's avahi name). Bare
     * "http://hub" is deliberately not attempted — Apple devices don't resolve
     * single-label names reliably; .local is the intended path. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("hub");
        mdns_instance_name_set("Better Robotics Hub");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS up: dashboard also at http://hub.local/");
    } else {
        ESP_LOGW(TAG, "mDNS init failed (http://hub.local won't resolve; IP still works)");
    }

    /* Bridge (httpd on :9001) starts first and returns; it dials the broker
     * lazily per browser, by which time the broker below is up. */
    start_ws_mqtt_bridge();

    /* Tier 3 (home mode, self_hub): this board self-hubbed with no hub present,
     * so it (a) watches briefly for a slow Pi to yield to, and (b) drives itself
     * via a local rover client against the broker below — a lone board is its own
     * hub AND rover. A forced hub (role_pref=HUB, tier 2 professor hub) does
     * neither: it was chosen deliberately (rebooting would re-force it, a loop),
     * and it must not drive (broker/AP vs real-time motors on one radio — hub#2).
     * Both tasks start before the broker call below blocks. */
    if (self_hub) {
        xTaskCreate(pi_watch_task, "pi-watch", 3072, NULL, 4, NULL);
        xTaskCreate(local_rover_task, "rover", 4096, NULL, 4, NULL);
    }

    /* Broker starts now, uplink or not — the classroom works offline. */
    struct mosq_broker_config bcfg = {
        .host = "0.0.0.0", .port = 1883, .tls_cfg = NULL, .handle_connect_cb = connect_cb,
    };
    ESP_LOGI(TAG, "starting broker on 0.0.0.0:1883 (raw MQTT; browsers reach it via the :9001 WS bridge)");
    mosq_broker_run(&bcfg); /* blocks */
}
