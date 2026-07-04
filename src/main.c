#include <stdio.h>
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
 * Mode dispatch is stateless: NVS holds only credentials, and behavior is a
 * pure function of "is config complete" plus a one-shot provision request.
 * The request rides RTC noinit RAM — it survives esp_restart() (how a mode
 * asks for the other one) but not a power cycle, so no robot can ever be
 * stranded in the wrong mode by stale state.
 *
 * The two radios never run in the same boot:
 *
 *   operating (Wi-Fi + zenoh) ── failure or button ──► provisioning window
 *   provisioning (BLE)        ── done, window expiry, or button ──► restart,
 *                                which retries the stored credentials
 */
#define PROVISION_MAGIC 0x50524f56u  /* "PROV" */
static RTC_NOINIT_ATTR uint32_t s_provision_request;

#define PROVISION_WINDOW_US (3 * 60 * 1000000LL)

static volatile bool s_operating = false;  /* which mode the button acts on */

/* ── BOOT button: hold ~1 s to switch modes ──────────────────────────────── */

#ifndef BUTTON_GPIO
#define BUTTON_GPIO GPIO_NUM_0   /* classic devkit BOOT; C3 SuperMini env passes GPIO_NUM_9 */
#endif

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

static void on_evt(void *a, esp_event_base_t base, int32_t id, void *d) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        s_got_ip = true;
}

static bool wifi_start(const char *ssid, const char *pass) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL);
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Wait up to 30 s; the event handler keeps reconnecting, so a brief AP
     * outage inside this window resolves without dropping to provisioning. */
    for (int i = 0; i < 120 && !s_got_ip; i++) vTaskDelay(pdMS_TO_TICKS(250));
    return s_got_ip;
}

static void operating_mode(const char *ssid, const char *pass, const char *locator) {
    ESP_LOGI(TAG, "operating mode — joining '%s', hub %s", ssid, locator);

    if (!wifi_start(ssid, pass)) {
        ESP_LOGE(TAG, "wifi join failed");
        goto fail;
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

    char key[48];
    snprintf(key, sizeof key, "robots/%s/sys", s_id);
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, key);

    z_owned_publisher_t pub;
    if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0) {
        ESP_LOGE(TAG, "declare_publisher failed");
        goto fail;  /* restart cleans up; z_close() not needed before esp_restart */
    }

    ESP_LOGI(TAG, "publishing %s every 2 s", key);

    char buf[160];
    int put_fails = 0;   /* consecutive failed puts — detects a dead session */
    for (;;) {
        int64_t up_ms = esp_timer_get_time() / 1000;
        uint32_t heap = esp_get_free_heap_size();
        snprintf(buf, sizeof buf,
                 "{\"uptime_ms\":%lld,\"free_heap\":%u,\"synthetic\":false}",
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
    ESP_LOGI(TAG, "provisioning window expired — retrying stored credentials");
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

static void provisioning_mode(bool have_config) {
    ESP_LOGI(TAG, "provisioning mode — advertising %s", s_id);
    if (have_config) {
        /* Credentials exist, so this is a fallback visit: bound it, then
         * reboot to retry — a transient outage self-heals with no human. */
        const esp_timer_create_args_t t = {.callback = window_expired, .name = "prov_window"};
        ESP_ERROR_CHECK(esp_timer_create(&t, &s_window));
        ESP_ERROR_CHECK(esp_timer_start_once(s_window, PROVISION_WINDOW_US));
    }
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

    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

    char ssid[33], pass[65], loc[65];
    bool have_config = rover_config_load(ssid, pass, loc);

    if (have_config && !provision_requested) {
        s_operating = true;
        operating_mode(ssid, pass, loc);   /* never returns */
    }
    provisioning_mode(have_config);
}
