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
 *   - AP  (hub-<suffix>)    : students/rovers/laptop join here. `hub-` prefix
 *                             matches the Pi's SSID convention so a rover's
 *                             `hub-*` scan finds either hub (CONTRACT.md).
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

/* STA_SSID / STA_PASS — the venue uplink. Kept in a gitignored header so real
 * Wi-Fi credentials never land in committed source; copy wifi_creds.example.h
 * to wifi_creds.h and fill it in. */
#include "wifi_creds.h"

/* --- AP: what students/rovers/laptop join (demo creds, safe to commit) --- */
#define AP_SSID_PREFIX "hub-"          /* + last 2 MAC bytes → hub-a3f2; the
                                        * `hub-*` convention a rover scans for */
#define AP_PASS     "brobotics"        /* 8-63 chars → WPA2; "" → open */
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

/* ── Abdication (DESIGN-unified.md § Hub election, lowest-MAC tiebreak + Pi
 * preference) ───────────────────────────────────────────────────────────────
 * An ESP that ELECTED itself hub keeps watching, for a bounded window, for a hub
 * it must yield to — the race is early: a Pi boots ~30-60 s after us, and a
 * simultaneous second claimant appears within a scan. Yield to any hub-pi-* (the
 * Pi is always preferred, whatever its MAC) or any hub-* with a lower BSSID (a
 * deterministic tiebreak so exactly one of two claimants survives). Yielding =
 * esp_restart: the election re-runs and we join the winner as a rover. Only an
 * *elected* hub does this — a forced role_pref=HUB rebooting would be re-forced
 * into the hub role, an infinite loop. Each active scan briefly blips the
 * single-radio AP, so the cadence is slow and the window closes once the race
 * is past. */
#define ABDICATE_WINDOW_MS  180000   /* watch the first ~3 min, then commit as the hub */
#define ABDICATE_SCAN_MS     20000   /* slow — an active scan interrupts AP+STA briefly */

static bool sees_hub_to_yield_to(const uint8_t self_bssid[6]) {
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    wifi_ap_record_t ap[32];
    uint16_t n = sizeof ap / sizeof ap[0];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) return false;
    for (int i = 0; i < n; i++) {
        const char *ss = (const char *)ap[i].ssid;
        if (strncmp(ss, HUB_SSID_PREFIX, sizeof HUB_SSID_PREFIX - 1) != 0) continue;
        if (memcmp(ap[i].bssid, self_bssid, 6) == 0) continue;   /* our own AP beacon */
        if (strncmp(ss, HUB_PI_SSID_PREFIX, sizeof HUB_PI_SSID_PREFIX - 1) == 0) {
            ESP_LOGW(TAG, "abdicate: Pi hub '%s' present — stepping down (Pi is preferred)", ss);
            return true;
        }
        if (memcmp(ap[i].bssid, self_bssid, 6) < 0) {   /* MACs compare MSB-first = numeric */
            ESP_LOGW(TAG, "abdicate: lower-MAC hub '%s' present — stepping down (tiebreak)", ss);
            return true;
        }
    }
    return false;
}

static void abdication_task(void *arg) {
    (void)arg;
    uint8_t self_bssid[6];
    esp_read_mac(self_bssid, ESP_MAC_WIFI_SOFTAP);
    for (uint32_t waited = 0; waited < ABDICATE_WINDOW_MS; waited += ABDICATE_SCAN_MS) {
        vTaskDelay(pdMS_TO_TICKS(ABDICATE_SCAN_MS));
        if (sees_hub_to_yield_to(self_bssid)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();   /* → dispatcher → election → join the winner as a rover */
        }
    }
    ESP_LOGI(TAG, "hub election settled — committed as the hub (abdication window closed)");
    vTaskDelete(NULL);
}

void hub_role_run(bool elected)   /* dispatched from main.c; never returns (blocks in the broker) */
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

    /* SSID = hub-<last 2 MAC bytes>, e.g. hub-a3f2 — unique per hub, shared
     * `hub-` prefix so a rover's `hub-*` scan finds either hub (CONTRACT.md). */
    uint8_t apmac[6];
    esp_read_mac(apmac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[16];
    int ap_ssid_len = snprintf(ap_ssid, sizeof ap_ssid,
                               AP_SSID_PREFIX "%02x%02x", apmac[4], apmac[5]);

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

    /* An elected hub watches briefly for a hub it should yield to (a slow Pi, or
     * a simultaneous second claimant) and steps down if one appears — DESIGN-
     * unified.md § Hub election. A forced hub (role_pref=HUB) never does: it was
     * chosen deliberately, and rebooting would only re-force it (a loop). */
    if (elected)
        xTaskCreate(abdication_task, "abdicate", 3072, NULL, 4, NULL);

    /* Broker starts now, uplink or not — the classroom works offline. */
    struct mosq_broker_config bcfg = {
        .host = "0.0.0.0", .port = 1883, .tls_cfg = NULL, .handle_connect_cb = connect_cb,
    };
    ESP_LOGI(TAG, "starting broker on 0.0.0.0:1883 (raw MQTT; browsers reach it via the :9001 WS bridge)");
    mosq_broker_run(&bcfg); /* blocks */
}
