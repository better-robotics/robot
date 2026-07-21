// M3 zenoh-pico rover spike — speaks the hub wire contract over Zenoh.
//
// Joins the hub AP, connects to zenohd on the gateway, and:
//   - publishes robots/<ID>/sys (presence) + robots/<ID>/imu (dummy) every 1s
//   - subscribes robots/<ID>/pwm      -> drives the onboard LED (motor stand-in)
//   - declares a queryable robots/<ID>/led -> replies {status:ok} (set_led RPC)
//   - subscribes fleet/estop          -> logs the latch
// Enough to prove the ESP<->zenohd<->bridge path on real hardware.

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "zenoh-pico.h"

#define WIFI_SSID "hub-f825"
#define WIFI_PASS ""                       // open hub AP
#define ZENOH_CONNECT "tcp/192.168.4.1:7447" // the DHCP gateway = the Pi = zenohd
#define ROVER_ID "rover-z"
#define LED_GPIO GPIO_NUM_2                // az-delivery devkit onboard LED

#define K_SYS "robots/" ROVER_ID "/sys"
#define K_IMU "robots/" ROVER_ID "/imu"
#define K_PWM "robots/" ROVER_ID "/pwm"
#define K_LED "robots/" ROVER_ID "/led"
#define K_ESTOP "fleet/estop"

static const char *TAG = "zrover";
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

// ---- Wi-Fi STA (open AP) ----------------------------------------------------
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW("zrover", "wifi disconnected, reason=%d", d ? d->reason : -1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_join(void) {
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL, NULL));
    wifi_config_t wc = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS,
                                .threshold = {.authmode = WIFI_AUTH_OPEN}}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // no NVS wifi cfg interference
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // brcmfmac AP drops power-saving STAs
    ESP_LOGI(TAG, "joining %s ...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "wifi up");
}

// ---- Zenoh handlers ---------------------------------------------------------
static void on_pwm(z_loaned_sample_t *sample, void *arg) {
    (void)arg;
    z_owned_string_t val;
    z_bytes_to_string(z_sample_payload(sample), &val);
    const char *s = z_string_data(z_string_loan(&val));
    size_t n = z_string_len(z_string_loan(&val));
    cJSON *root = cJSON_ParseWithLength(s, n);
    int l = 0, r = 0;
    if (root) {
        cJSON *jl = cJSON_GetObjectItem(root, "left_motor");
        cJSON *jr = cJSON_GetObjectItem(root, "right_motor");
        if (cJSON_IsNumber(jl)) l = jl->valueint;
        if (cJSON_IsNumber(jr)) r = jr->valueint;
        cJSON_Delete(root);
    }
    gpio_set_level(LED_GPIO, (l != 0 || r != 0) ? 1 : 0);
    ESP_LOGI(TAG, "pwm L=%d R=%d -> LED %s", l, r, (l || r) ? "ON" : "off");
    z_drop(z_move(val));
}

static void on_estop(z_loaned_sample_t *sample, void *arg) {
    (void)arg;
    z_owned_string_t val;
    z_bytes_to_string(z_sample_payload(sample), &val);
    cJSON *root = cJSON_ParseWithLength(z_string_data(z_string_loan(&val)), z_string_len(z_string_loan(&val)));
    bool engaged = true; // parse-failure fails toward stopped (contract)
    if (root) {
        cJSON *je = cJSON_GetObjectItem(root, "engaged");
        if (cJSON_IsBool(je)) engaged = cJSON_IsTrue(je);
        cJSON_Delete(root);
    }
    ESP_LOGW(TAG, "fleet/estop -> %s", engaged ? "ENGAGED" : "clear");
    z_drop(z_move(val));
}

static void on_led_query(z_loaned_query_t *query, void *ctx) {
    (void)ctx;
    z_owned_string_t req;
    z_bytes_to_string(z_query_payload(query), &req);
    ESP_LOGI(TAG, "led query: %.*s", (int)z_string_len(z_string_loan(&req)), z_string_data(z_string_loan(&req)));
    z_drop(z_move(req));
    z_owned_bytes_t reply;
    z_bytes_copy_from_str(&reply, "{\"status\":\"ok\"}");
    z_query_reply(query, z_query_keyexpr(query), z_move(reply), NULL);
}

static void put_json(z_loaned_session_t *s, const char *keyexpr, const char *json) {
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, keyexpr);
    z_owned_bytes_t payload;
    z_bytes_copy_from_str(&payload, json);
    z_put(s, z_loan(ke), z_move(payload), NULL);
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
    gpio_set_level(LED_GPIO, 0);

    wifi_join();

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, ZENOH_CONNECT);

    ESP_LOGI(TAG, "opening zenoh session -> %s", ZENOH_CONNECT);
    z_owned_session_t s;
    z_open_options_t oopts;
    z_open_options_default(&oopts);
    oopts.auto_start_read_task = false;  // start tasks explicitly below (auto_start didn't run them)
    oopts.auto_start_lease_task = false;
    if (z_open(&s, z_move(config), &oopts) < 0) {
        ESP_LOGE(TAG, "z_open failed — is zenohd up on the gateway?");
        esp_restart();
    }
    ESP_LOGI(TAG, "zenoh session open");
    // Read task processes INCOMING data (subs/queryable); lease task keeps the
    // session alive. Without the read task the rover can publish but never receive.
    if (zp_start_read_task(z_loan_mut(s), NULL) < 0 || zp_start_lease_task(z_loan_mut(s), NULL) < 0) {
        ESP_LOGE(TAG, "failed to start read/lease tasks — Z_FEATURE_MULTI_THREAD off?");
        esp_restart();
    }
    ESP_LOGI(TAG, "read + lease tasks started");

    // Subscribers
    z_owned_subscriber_t sub_pwm, sub_estop;
    z_owned_closure_sample_t cb_pwm, cb_estop;
    z_view_keyexpr_t ke;

    z_closure(&cb_pwm, on_pwm, NULL, NULL);
    z_view_keyexpr_from_str_unchecked(&ke, K_PWM);
    z_declare_subscriber(z_loan(s), &sub_pwm, z_loan(ke), z_move(cb_pwm), NULL);

    z_closure(&cb_estop, on_estop, NULL, NULL);
    z_view_keyexpr_from_str_unchecked(&ke, K_ESTOP);
    z_declare_subscriber(z_loan(s), &sub_estop, z_loan(ke), z_move(cb_estop), NULL);

    // Queryable for set_led
    z_owned_queryable_t qab;
    z_owned_closure_query_t cb_q;
    z_closure(&cb_q, on_led_query, NULL, NULL);
    z_view_keyexpr_from_str_unchecked(&ke, K_LED);
    z_declare_queryable(z_loan(s), &qab, z_loan(ke), z_move(cb_q), NULL);

    ESP_LOGI(TAG, "declared: sub %s, sub %s, queryable %s", K_PWM, K_ESTOP, K_LED);

    char buf[192];
    for (uint32_t i = 0;; i++) {
        int64_t t = esp_timer_get_time() / 1000000;
        snprintf(buf, sizeof buf,
                 "{\"timestamp\":%lld,\"board\":\"%s\",\"free_heap\":%u,\"uptime\":%u}",
                 (long long)t, ROVER_ID, (unsigned)esp_get_free_heap_size(), (unsigned)i);
        put_json(z_loan(s), K_SYS, buf);
        snprintf(buf, sizeof buf,
                 "{\"timestamp\":%lld,\"accel_x\":0.0,\"accel_y\":0.0,\"accel_z\":9.81,"
                 "\"gyro_x\":0.0,\"gyro_y\":0.0,\"gyro_z\":0.0}",
                 (long long)t);
        put_json(z_loan(s), K_IMU, buf);
        if (i % 5 == 0) ESP_LOGI(TAG, "beacon %u (heap %u)", (unsigned)i, (unsigned)esp_get_free_heap_size());
        sleep(1);
    }
}
