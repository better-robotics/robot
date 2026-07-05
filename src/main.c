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
#include "zenoh-pico.h"
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
 *   operating (Wi-Fi + zenoh) ── failure, button, or fabric command ──► provisioning window
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

/* ── operating mode: Wi-Fi STA + zenoh client ────────────────────────────── */

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

/* Fabric-triggered re-entry: publish anything to robots/<id>/cmd/reprovision
 * (cmd/<verb> is the command plane — future verbs sit beside it) and
 * the rover reboots into a provisioning window. The BOOT button's remote
 * twin — and the only re-entry an ESP32-CAM has (no button at all). */
static void on_reprovision(z_loaned_sample_t *sample, void *arg) {
    (void)sample; (void)arg;
    ESP_LOGW(TAG, "reprovision command — opening a provisioning window");
    s_provision_request = PROVISION_MAGIC;
    esp_restart();
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

    char derived[65];
    if (!locator[0] || on_discovered) {
        /* The discovered network's hub IS its gateway (NM shared mode); and a
         * stored locator, when we're here via stale-config fallback, is as
         * stale as the ssid it was written alongside. */
        snprintf(derived, sizeof derived, "tcp/" IPSTR ":7447", IP2STR(&s_gw));
        locator = derived;
        ESP_LOGI(TAG, "hub address derived from gateway: %s", locator);
    }

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, locator);
    /* No auth: zenoh-pico 1.9 declares Z_CONFIG_USER/PASSWORD_KEY but its
     * transport never implements the usrpwd extension, so a usrpwd-enforcing
     * router rejects the session outright (verified on hardware 2026-07-04).
     * MCU identity needs TLS certs or endpoint segregation — design open. */

    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) {
        ESP_LOGE(TAG, "z_open failed -> %s", locator);
        goto fail;
    }

    /* The read task is what makes the session bidirectional — without it the
     * reprovision subscriber below never sees a sample. Lease keepalives ride
     * their own task; publish-only firmware got by on put traffic alone. */
    if (zp_start_read_task(z_loan_mut(s), NULL) < 0 ||
        zp_start_lease_task(z_loan_mut(s), NULL) < 0)
        ESP_LOGW(TAG, "transport tasks failed — remote reprovision unavailable");

    char key[48];
    snprintf(key, sizeof key, "robots/%s/sys", s_id);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, key);

    z_owned_publisher_t pub;
    if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0) {
        ESP_LOGE(TAG, "declare_publisher failed");
        goto fail;  /* restart cleans up; z_close() not needed before esp_restart */
    }

    char rkey[48];
    snprintf(rkey, sizeof rkey, "robots/%s/cmd/reprovision", s_id);
    z_view_keyexpr_t rke;
    z_view_keyexpr_from_str(&rke, rkey);
    z_owned_closure_sample_t rcb;
    z_closure(&rcb, on_reprovision, NULL, NULL);
    z_owned_subscriber_t sub;
    if (z_declare_subscriber(z_loan(s), &sub, z_loan(rke), z_move(rcb), NULL) < 0)
        ESP_LOGW(TAG, "declare_subscriber failed — remote reprovision unavailable");

    ESP_LOGI(TAG, "publishing %s every 2 s", key);

    char buf[160];
    int put_fails = 0;   /* consecutive failed puts — detects a dead session */
    for (;;) {
        int64_t up_ms = esp_timer_get_time() / 1000;
        uint32_t heap = esp_get_free_heap_size();
        snprintf(buf, sizeof buf,
                 "{\"uptime_ms\":%lld,\"free_heap\":%u,\"hw\":\"" HW_BOARD "\",\"synthetic\":false}",
                 (long long)up_ms, (unsigned)heap);
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, buf);
        if (z_publisher_put(z_loan(pub), z_move(payload), NULL) < 0) {
            if (++put_fails >= 5) {
                ESP_LOGE(TAG, "5 consecutive put failures — session dead");
                goto fail;
            }
            ESP_LOGW(TAG, "publish failed (%d/5)", put_fails);
        } else {
            put_fails = 0;
            ESP_LOGI(TAG, "pub %s", buf);
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
