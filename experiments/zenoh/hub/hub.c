// Stage 2 probe — the ESP32-C3 as a zenoh-pico HUB (no Pi, no zenohd).
//
// Creates an open SoftAP "hub-c3xx"; runs zenoh-pico in PEER mode with a TCP
// listen endpoint on :7447 so rovers can connect to it directly; subscribes
// robots/** and logs what arrives. This probes the council's open question:
// can zenoh-pico on an MCU accept incoming connections and act as the star
// center? If a rover joins the AP and its telemetry shows up here, yes.

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "zenoh-pico.h"

#define AP_CHANNEL 6
#define LED_GPIO GPIO_NUM_8   // C3 SuperMini onboard LED (active low)
static const char *TAG = "zhub";

static void softap_start(char *ssid_out) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    sprintf(ssid_out, "hub-%02x%02x", mac[4], mac[5]);

    wifi_config_t ap = {.ap = {.channel = AP_CHANNEL, .max_connection = 4,
                               .authmode = WIFI_AUTH_OPEN}};
    size_t n = strlen(ssid_out);
    memcpy(ap.ap.ssid, ssid_out, n);
    ap.ap.ssid_len = n;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // no dozing — this is the hub
    ESP_LOGI(TAG, "SoftAP up: SSID=%s  gateway=192.168.4.1  (rovers connect tcp/192.168.4.1:7447)", ssid_out);
}

static void on_robots(z_loaned_sample_t *sample, void *arg) {
    (void)arg;
    z_view_string_t ks;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks);
    z_owned_string_t val;
    z_bytes_to_string(z_sample_payload(sample), &val);
    ESP_LOGI(TAG, "RX %.*s = %.*s",
             (int)z_string_len(z_view_string_loan(&ks)), z_string_data(z_view_string_loan(&ks)),
             (int)z_string_len(z_string_loan(&val)), z_string_data(z_string_loan(&val)));
    z_drop(z_move(val));
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 1);  // off (active low)

    char ssid[16];
    softap_start(ssid);

    // zenoh-pico peer, listening for rover connections on the SoftAP.
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, "tcp/0.0.0.0:7447");

    ESP_LOGI(TAG, "opening zenoh peer (listen tcp/0.0.0.0:7447)...");
    z_owned_session_t s;
    z_open_options_t oo;
    z_open_options_default(&oo);
    oo.auto_start_read_task = false;
    oo.auto_start_lease_task = false;
    if (z_open(&s, z_move(config), &oo) < 0) {
        ESP_LOGE(TAG, "z_open (peer/listen) failed");
        esp_restart();
    }
    if (zp_start_read_task(z_loan_mut(s), NULL) < 0 || zp_start_lease_task(z_loan_mut(s), NULL) < 0) {
        ESP_LOGE(TAG, "failed to start read/lease tasks");
        esp_restart();
    }
    ESP_LOGI(TAG, "zenoh peer open + tasks started");

    z_owned_closure_sample_t cb;
    z_closure(&cb, on_robots, NULL, NULL);
    z_owned_subscriber_t sub;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, "robots/**");
    if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(cb), NULL) < 0) {
        ESP_LOGE(TAG, "declare subscriber robots/** failed");
        esp_restart();
    }
    ESP_LOGI(TAG, "subscribed robots/** — waiting for rovers to connect and publish");

    z_view_keyexpr_t pwmke;
    z_view_keyexpr_from_str_unchecked(&pwmke, "robots/rover-z/pwm");
    for (uint32_t i = 0;; i++) {
        wifi_sta_list_t sta;
        esp_wifi_ap_get_sta_list(&sta);
        gpio_set_level(LED_GPIO, (i & 1) ? 1 : 0);  // heartbeat blink
        if (i % 5 == 0) ESP_LOGI(TAG, "hub alive: %d station(s) on the AP, heap %u",
                                 sta.num, (unsigned)esp_get_free_heap_size());
        // DOWNLINK PROOF: the hub commands the rover every 4 s (drive, then stop).
        if (i % 4 == 0) {
            int v = (i % 8 == 0) ? 180 : 0;
            char buf[96];
            snprintf(buf, sizeof buf, "{\"timestamp\":%u,\"left_motor\":%d,\"right_motor\":%d,\"duration_ms\":3000}",
                     (unsigned)i, v, v);
            z_owned_bytes_t p;
            z_bytes_copy_from_str(&p, buf);
            z_put(z_loan(s), z_loan(pwmke), z_move(p), NULL);
            ESP_LOGI(TAG, "TX robots/rover-z/pwm L=%d R=%d (hub commanding the rover)", v, v);
        }
        sleep(1);
    }
}
