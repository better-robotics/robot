// rover-zenoh v1 — classic ESP32 publishes real system telemetry to the hub
// over zenoh-pico (client mode, TCP). WiFi: DukeVisitor (open). Telemetry is
// genuinely measured (free heap + uptime); chip temp + the led queryable come
// in v2. No IMU is faked (this board has none).

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "zenoh-pico.h"

#define WIFI_SSID   "DukeVisitor"
#define WIFI_PASS   ""                       // open network
#define HUB_LOCATOR "tcp/172.28.143.53:7447" // the Mac's hubd (open router)
#define ROBOT_ID    "esp32_01"

static const char *TAG = "rover-zenoh";
static volatile bool s_got_ip = false;

static void on_evt(void *a, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected — retrying");
        s_got_ip = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "GOT IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
    }
}

static void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL);
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    wifi_start();
    ESP_LOGI(TAG, "joining '%s' ...", WIFI_SSID);
    while (!s_got_ip) vTaskDelay(pdMS_TO_TICKS(250));

    // --- zenoh client -> hub router ---
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, HUB_LOCATOR);
    ESP_LOGI(TAG, "opening zenoh session -> %s", HUB_LOCATOR);

    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) {
        ESP_LOGE(TAG, "z_open failed — is hubd up at %s?", HUB_LOCATOR);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    ESP_LOGI(TAG, "zenoh session UP (read/lease tasks auto-started)");

    // --- publisher on robots/<id>/sys ---
    z_owned_publisher_t pub;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "robots/" ROBOT_ID "/sys");
    if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0) {
        ESP_LOGE(TAG, "declare_publisher failed");
        esp_restart();
    }
    ESP_LOGI(TAG, "publishing robots/%s/sys every 2s", ROBOT_ID);

    char buf[160];
    for (;;) {
        int64_t up_ms = esp_timer_get_time() / 1000;     // real: uptime
        uint32_t heap = esp_get_free_heap_size();         // real: free heap
        snprintf(buf, sizeof buf,
                 "{\"uptime_ms\":%lld,\"free_heap\":%u,\"synthetic\":false}",
                 (long long)up_ms, (unsigned)heap);
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, buf);
        z_publisher_put(z_loan(pub), z_move(payload), NULL);
        ESP_LOGI(TAG, "pub %s", buf);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
