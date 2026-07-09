#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "rover_config.h"
#include "provisioning_util.h"
#include "provisioning.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "rover";
static char s_id[16];

/*
 * Mode dispatch is stateless: NVS holds only explicit choices, and behavior
 * is a pure function of that config plus a one-shot provision request. The
 * request rides RTC noinit RAM — it survives esp_restart() (how a mode asks
 * for the other one) but not a power cycle, so no robot can ever be stranded
 * in the wrong mode by stale state.
 *
 * Nothing stored is a fully operable state: no ssid → scan-join the strongest
 * open hub-* network (the classroom AP convention IS the onboarding channel),
 * no locator → dial the network gateway (on its own AP the hub is the
 * gateway). BLE provisioning is the override path, not the front door.
 *
 * The two radios never run in the same boot:
 *
 *   operating (Wi-Fi + MQTT)  ── failure, button, or fabric command ──► provisioning window
 *   provisioning (BLE)        ── done, window expiry, or button ──► restart into operating,
 *                                which retries stored credentials or re-scans for a hub
 */
#define PROVISION_MAGIC 0x50524f56u  /* "PROV" */
static RTC_NOINIT_ATTR uint32_t s_provision_request;

#define PROVISION_WINDOW_US (3 * 60 * 1000000LL)

static volatile bool s_operating = false;  /* which mode the button acts on */

/* ── BOOT button: hold ~1 s to switch modes ──────────────────────────────── */

#ifndef BUTTON_GPIO
#define BUTTON_GPIO GPIO_NUM_0   /* classic devkit BOOT; C3 SuperMini env passes GPIO_NUM_9 */
#endif

/* ── onboard LED: lit while provisioning ─────────────────────────────────── */

#ifndef LED_GPIO
#define LED_GPIO GPIO_NUM_2      /* classic devkit LED; C3 SuperMini env passes GPIO_NUM_8 */
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0         /* SuperMini's LED sinks into the pin — env passes 1 */
#endif

/* Entering provisioning mode is otherwise invisible from the outside — the
 * first field test of the BOOT button read as "nothing happened" while the
 * window was in fact open (2026-07-04). LED on = BLE window open. */
static void led_set(bool on) {
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? !on : on);
}

static void button_task(void *p) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held = 0;
    for (;;) {
        held = (gpio_get_level(BUTTON_GPIO) == 0) ? held + 1 : 0;
        if (held >= 10) {
            ESP_LOGI(TAG, "button held — switching to %s",
                     s_operating ? "provisioning" : "operating");
            if (s_operating) s_provision_request = PROVISION_MAGIC;
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── operating mode: Wi-Fi STA + esp-mqtt client ─────────────────────────── */

static volatile bool s_got_ip = false;
static volatile bool s_want_connect = false;   /* gates auto-reconnect during scans */
static esp_ip4_addr_t s_gw;                    /* DHCP gateway — on the hub's AP, the hub */

static void on_evt(void *a, esp_event_base_t base, int32_t id, void *d) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        if (s_want_connect) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_gw = ((ip_event_got_ip_t *)d)->ip_info.gw;
        s_got_ip = true;
    }
}

static void wifi_up(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wifi_join(const char *ssid, const char *pass) {
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_want_connect = true;
    esp_wifi_connect();
    /* Wait up to 30 s; the event handler keeps reconnecting, so a brief AP
     * outage inside this window resolves without dropping to provisioning. */
    for (int i = 0; i < 120 && !s_got_ip; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_got_ip) { s_want_connect = false; esp_wifi_disconnect(); }
    return s_got_ip;
}

/* Zero-touch onboarding: an OPEN network named hub-* is the classroom
 * convention, so its existence is all the configuration a rover needs.
 * Strongest wins when several are in range. */
static bool discover_hub(char out[33]) {
    ESP_LOGI(TAG, "scanning for an open hub-* network");
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    wifi_ap_record_t *ap = malloc(n * sizeof *ap);
    if (!ap) return false;
    esp_wifi_scan_get_ap_records(&n, ap);
    int best = -1;
    for (int i = 0; i < n; i++)
        if (ap[i].authmode == WIFI_AUTH_OPEN &&
            strncmp((const char *)ap[i].ssid, "hub-", 4) == 0 &&
            (best < 0 || ap[i].rssi > ap[best].rssi))
            best = i;
    if (best >= 0) {
        snprintf(out, 33, "%s", (const char *)ap[best].ssid);
        ESP_LOGI(TAG, "found %s (%d dBm)", out, ap[best].rssi);
    }
    free(ap);
    return best >= 0;
}

/* MQTT identity. robot-id == the team credential (CONTRACT.md § Discovery &
 * isolation): the rover authenticates as its team and publishes under its own
 * robots/<team> subtree, so the Pi's `pattern robots/%u/#` ACL admits it and a
 * team can't touch another's subtree. The MAC-derived s_id stays the BLE adv
 * name + a payload field — hardware is metadata, never the topic id.
 * Demo defaults; real per-team creds arrive via provisioning (hub#1 follow-up). */
#ifndef MQTT_USER
#define MQTT_USER "team1"
#endif
#ifndef MQTT_PASS
#define MQTT_PASS "change-me-team1"
#endif
static const char *s_topic_id = MQTT_USER;

static volatile bool s_mqtt_up = false;   /* live session; drives dead-session self-heal */

/* Fabric-triggered re-entry: publish anything to robots/<id>/cmd/reprovision
 * (cmd/<verb> is the command plane) and the rover reboots into a provisioning
 * window. The BOOT button's remote twin — the only re-entry an ESP32-CAM has.
 * We subscribe only that one topic, so any MQTT_EVENT_DATA is the reprovision. */
static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_mqtt_up = true;
        char rkey[48];
        snprintf(rkey, sizeof rkey, "robots/%s/cmd/reprovision", s_topic_id);
        esp_mqtt_client_subscribe(e->client, rkey, 0);
        ESP_LOGI(TAG, "mqtt connected; subscribed %s", rkey);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGW(TAG, "reprovision command — opening a provisioning window");
        s_provision_request = PROVISION_MAGIC;
        esp_restart();
        break;
    default:
        break;
    }
}

static void operating_mode(char *ssid, const char *pass, const char *locator) {
    wifi_up();

    char discovered[33] = "";
    bool on_discovered = false;
    if (!ssid[0]) {
        if (!discover_hub(discovered)) {
            ESP_LOGW(TAG, "nothing stored and no open hub-* in range");
            goto fail;
        }
        ssid = discovered; pass = ""; on_discovered = true;
    }

    ESP_LOGI(TAG, "operating mode — joining '%s'", ssid);
    if (!wifi_join(ssid, pass)) {
        ESP_LOGE(TAG, "wifi join failed");
        /* Stored credentials can go stale (hub swapped, AP renamed) — a live
         * open hub-* beats a dead config. Discovery is never persisted: NVS
         * keeps explicit choices only, so the stored pair is retried first
         * on every boot. */
        if (!on_discovered && discover_hub(discovered)
            && strcmp(ssid, discovered) != 0 && wifi_join(discovered, "")) {
            ssid = discovered; on_discovered = true;
        } else {
            goto fail;
        }
    }

    /* Broker URI. Gateway-first (CONTRACT.md): on the hub's own AP the DHCP
     * gateway IS the hub, so <gateway>:1883 reaches the broker with no name
     * lookup and no hardcoded IP — the one address the two hub hosts (Pi,
     * ESP32) don't share. A stored locator overrides: a full mqtt:// URI is
     * used as-is, a bare host becomes mqtt://<host>:1883. */
    char uri[80];
    if (locator[0] && !on_discovered) {
        if (strncmp(locator, "mqtt://", 7) == 0)
            snprintf(uri, sizeof uri, "%s", locator);
        else
            snprintf(uri, sizeof uri, "mqtt://%s:1883", locator);
    } else {
        snprintf(uri, sizeof uri, "mqtt://" IPSTR ":1883", IP2STR(&s_gw));
    }
    ESP_LOGI(TAG, "broker %s as '%s'", uri, MQTT_USER);

    /* esp-mqtt authenticates with username/password — the capability
     * zenoh-pico lacked (usrpwd unimplemented), and the reason the rover ships
     * on MQTT. Same firmware reaches either hub: both are raw-TCP brokers on
     * :1883, and a rover never needs the WebSocket transport (that's the
     * browser's constraint). */
    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = uri,
        .credentials = {
            .username = MQTT_USER,
            .authentication.password = MQTT_PASS,
        },
        .session.keepalive = 15,
    };
    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&mcfg);
    if (!cli) { ESP_LOGE(TAG, "mqtt init failed"); goto fail; }
    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    esp_mqtt_client_start(cli);

    /* First connect gates everything: it proves both reachability AND that the
     * team credential was accepted (a bad password disconnects here, never
     * reaching s_mqtt_up). No connect in 10 s → dead → provisioning window. */
    for (int i = 0; i < 40 && !s_mqtt_up; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_mqtt_up) { ESP_LOGE(TAG, "broker unreachable or credential rejected"); goto fail; }

    char key[48];
    snprintf(key, sizeof key, "robots/%s/sys", s_topic_id);
    ESP_LOGI(TAG, "publishing %s every 2 s", key);

    char buf[192];
    int down = 0;   /* consecutive 2 s ticks with no live session — dead → reprovision */
    for (;;) {
        if (s_mqtt_up) {
            down = 0;
            int64_t up_ms = esp_timer_get_time() / 1000;
            uint32_t heap = esp_get_free_heap_size();
            /* board id is metadata in the payload, not the topic (id == team). */
            snprintf(buf, sizeof buf,
                     "{\"uptime_ms\":%lld,\"free_heap\":%u,\"hw\":\"" HW_BOARD
                     "\",\"board\":\"%s\",\"synthetic\":false}",
                     (long long)up_ms, (unsigned)heap, s_id);
            if (esp_mqtt_client_publish(cli, key, buf, 0, 0, 0) >= 0)
                ESP_LOGI(TAG, "pub %s", buf);
            else
                ESP_LOGW(TAG, "publish enqueue failed");
        } else if (++down >= 10) {
            /* esp-mqtt auto-reconnects, so a brief hub outage self-heals; only
             * a sustained dead session (~20 s) drops to a provisioning window. */
            ESP_LOGE(TAG, "20 s with no broker — session dead");
            goto fail;
        } else {
            ESP_LOGW(TAG, "waiting for reconnect (%d/10)", down);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

fail:
    ESP_LOGW(TAG, "falling back to a provisioning window");
    s_provision_request = PROVISION_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ── provisioning mode: BLE (Improv Wi-Fi + hubcfg locator) ─────────────── */

static esp_timer_handle_t s_window;

static void window_expired(void *p) {
    if (provisioning_client_connected()) {
        /* Someone is mid-provisioning — give them another window. */
        esp_timer_start_once(s_window, PROVISION_WINDOW_US);
        return;
    }
    ESP_LOGI(TAG, "provisioning window expired — rebooting into operating mode");
    esp_restart();
}

/* Reboot-on-complete is debounced and deferred while a BLE client is attached:
 * s_done_cb fires in GATT write context, and an immediate restart there races
 * the peer's next operation (read-back, second write, Improv's PROVISIONED
 * notify) — the client sees "Device disconnected" mid-exchange. */
static esp_timer_handle_t s_done_reboot;
#define DONE_REBOOT_DELAY_US (4 * 1000000LL)

static void done_reboot(void *p) {
    if (provisioning_client_connected()) {
        esp_timer_start_once(s_done_reboot, DONE_REBOOT_DELAY_US);
        return;
    }
    ESP_LOGI(TAG, "provisioning complete — rebooting into operating mode");
    esp_restart();
}

static void on_provision_done(void) {
    if (!s_done_reboot) {
        const esp_timer_create_args_t t = {.callback = done_reboot, .name = "prov_done"};
        ESP_ERROR_CHECK(esp_timer_create(&t, &s_done_reboot));
    }
    esp_timer_stop(s_done_reboot);   /* no-op if not armed */
    ESP_ERROR_CHECK(esp_timer_start_once(s_done_reboot, DONE_REBOOT_DELAY_US));
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    provisioning_advertise(s_id);
}

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void provisioning_mode(void) {
    ESP_LOGI(TAG, "provisioning mode — advertising %s", s_id);
    led_set(true);
    /* Always bounded: expiry reboots into operating mode, which retries the
     * stored credentials or re-scans for a hub. A rover powered on before its
     * hub alternates scan → window until the hub appears — no human needed. */
    const esp_timer_create_args_t t = {.callback = window_expired, .name = "prov_window"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_window));
    ESP_ERROR_CHECK(esp_timer_start_once(s_window, PROVISION_WINDOW_US));
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(s_id);
    provisioning_register();
    provisioning_set_done_cb(on_provision_done);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    /* FreeRTOS tasks now running; app_main returns and the idle task takes over */
}

/* ── app_main: dispatch to exactly one radio path ────────────────────────── */

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    rover_format_robot_id(mac, s_id);
    ESP_LOGI(TAG, "robot id: %s", s_id);

    bool provision_requested = (s_provision_request == PROVISION_MAGIC);
    s_provision_request = 0;

    led_set(false);   /* also pins the active-low pin high so it can't glow half-lit */
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

    char ssid[33], pass[65], loc[65];
    rover_config_load(ssid, pass, loc);

    if (!provision_requested) {
        s_operating = true;
        operating_mode(ssid, pass, loc);   /* never returns */
    }
    provisioning_mode();
}
