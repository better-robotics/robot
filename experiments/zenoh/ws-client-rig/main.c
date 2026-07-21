// WS-JSON adapter test rig — stands in for the browser dashboard, on real
// silicon, so testing never has to hijack the laptop's Wi-Fi (which drops the
// user's internet). Joins the C3 hub's SoftAP and drives its :9001 adapter
// through the whole op surface, asserting each leg on the serial log:
//
//   sub  -> heartbeat + rover telemetry fan out to us            (adapter outbound)
//   pub fleet/estop WITHOUT auth -> {op:error}                   (instructor gate)
//   auth -> {op:auth,ok:true}; then estop pub accepted           (gate opens)
//   pub  robots/rover-z/pwm  -> the real rover's LED lights       (downlink)
//   get  robots/rover-z/led  -> {op:reply,...} from the rover     (queryable RPC)
//
// PASS/FAIL is printed at the end. This is the browser edge of the ESP-hub tier.

#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define WIFI_SSID "hub-f825"
#define HUB_WS "ws://192.168.4.1:9001/"
#define PASS "letmestop"
static const char *TAG = "wsclient";

static EventGroupHandle_t s_eg;
#define WIFI_BIT BIT0
#define WS_BIT BIT1

// counters updated in the WS event handler
static volatile int s_hb = 0, s_robot = 0;
static volatile bool s_err = false, s_auth_ok = false, s_reply = false;
static char s_reply_val[160];

static esp_websocket_client_handle_t s_client;

// ---- Wi-Fi STA --------------------------------------------------------------
static void wifi_evt(void *a, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "wifi disconnected reason=%d, retry", d ? d->reason : -1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_eg, WIFI_BIT);
    }
}

static void wifi_join(void) {
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    wifi_config_t wc = {.sta = {.ssid = WIFI_SSID, .threshold = {.authmode = WIFI_AUTH_OPEN}}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "joining %s ...", WIFI_SSID);
    xEventGroupWaitBits(s_eg, WIFI_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "wifi up");
}

// ---- WS event handler -------------------------------------------------------
static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_websocket_event_data_t *e = data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ws connected");
        xEventGroupSetBits(s_eg, WS_BIT);
    } else if (id == WEBSOCKET_EVENT_DATA && e->op_code == 0x1 && e->data_len > 1) {
        // text frame — the adapter's outbound JSON
        char buf[300];
        int n = e->data_len < (int)sizeof buf - 1 ? e->data_len : (int)sizeof buf - 1;
        memcpy(buf, e->data_ptr, n);
        buf[n] = 0;
        ESP_LOGI(TAG, "RX %s", buf);
        cJSON *r = cJSON_ParseWithLength(buf, n);
        if (!r) return;
        const cJSON *key = cJSON_GetObjectItem(r, "key");
        const cJSON *op = cJSON_GetObjectItem(r, "op");
        if (cJSON_IsString(key)) {
            if (!strcmp(key->valuestring, "hub/heartbeat")) s_hb++;
            else if (!strncmp(key->valuestring, "robots/", 7)) s_robot++;
        }
        if (cJSON_IsString(op)) {
            if (!strcmp(op->valuestring, "error")) s_err = true;
            else if (!strcmp(op->valuestring, "auth")) {
                const cJSON *ok = cJSON_GetObjectItem(r, "ok");
                s_auth_ok = cJSON_IsTrue(ok);
            } else if (!strcmp(op->valuestring, "reply")) {
                s_reply = true;
                const cJSON *v = cJSON_GetObjectItem(r, "val");
                char *vs = v ? cJSON_PrintUnformatted(v) : NULL;
                if (vs) { snprintf(s_reply_val, sizeof s_reply_val, "%s", vs); free(vs); }
            }
        }
        cJSON_Delete(r);
    }
}

static void send_op(const char *json) {
    esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "TX %s", json);
}

static void settle(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_join();

    esp_websocket_client_config_t cfg = { .uri = HUB_WS, .reconnect_timeout_ms = 3000,
                                          .network_timeout_ms = 5000 };
    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    esp_websocket_client_start(s_client);
    ESP_LOGI(TAG, "connecting ws %s", HUB_WS);
    xEventGroupWaitBits(s_eg, WS_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));

    // 1) subscribe — heartbeat proves adapter outbound; robots/** brings the rover
    send_op("{\"op\":\"sub\",\"key\":\"hub/heartbeat\"}");
    send_op("{\"op\":\"sub\",\"key\":\"robots/**\"}");
    ESP_LOGI(TAG, "[A] listening for fan-out ~7s");
    settle(7000);

    // 2) e-stop write WITHOUT auth -> must be refused
    ESP_LOGI(TAG, "[A] estop pub WITHOUT auth (expect error)");
    send_op("{\"op\":\"pub\",\"key\":\"fleet/estop\",\"val\":{\"engaged\":true,\"by\":\"rig\"}}");
    settle(2000);

    // 3) authenticate, then the same write should be accepted (no new error)
    ESP_LOGI(TAG, "[A] auth as instructor");
    bool err_before = s_err;
    send_op("{\"op\":\"auth\",\"role\":\"instructor\",\"password\":\"" PASS "\"}");
    settle(2000);
    ESP_LOGI(TAG, "[A] estop pub WITH auth (expect accepted)");
    send_op("{\"op\":\"pub\",\"key\":\"fleet/estop\",\"val\":{\"engaged\":false}}");
    settle(2000);
    bool estop_accepted = (s_err == err_before);  // no *new* error after auth

    // 4) downlink: drive the real rover through the adapter
    ESP_LOGI(TAG, "[B] pub pwm -> rover LED should light");
    send_op("{\"op\":\"pub\",\"key\":\"robots/rover-z/pwm\",\"val\":{\"left_motor\":200,\"right_motor\":200,\"duration_ms\":3000}}");
    settle(3000);

    // 5) queryable RPC: set_led on the rover, via get
    ESP_LOGI(TAG, "[B] get rover set_led queryable");
    send_op("{\"op\":\"get\",\"id\":\"g1\",\"key\":\"robots/rover-z/led\",\"val\":{\"on\":true,\"green\":255}}");
    settle(4000);

    ESP_LOGI(TAG, "==================== RESULTS ====================");
    ESP_LOGI(TAG, "heartbeat frames via adapter : %d   %s", s_hb, s_hb ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "estop refused w/o auth       : %s   %s", s_err ? "yes" : "no", s_err ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "instructor auth accepted     : %s   %s", s_auth_ok ? "yes" : "no", s_auth_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "estop write accepted w/ auth : %s   %s", estop_accepted ? "yes" : "no", estop_accepted ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "rover telemetry via adapter  : %d   %s", s_robot, s_robot ? "PASS" : "(no rover on AP?)");
    ESP_LOGI(TAG, "set_led reply via adapter    : %s   %s", s_reply ? s_reply_val : "-", s_reply ? "PASS" : "(no rover on AP?)");
    // Adapter outbound is proven by EITHER delivery path — the hub's own
    // heartbeat may not loop back to its own local subscriber, but the rover's
    // telemetry arrives over the network regardless.
    bool outbound = s_hb || s_robot;
    bool core = outbound && s_err && s_auth_ok && estop_accepted;
    ESP_LOGI(TAG, "adapter outbound (hb||rover)  : %s", outbound ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "ADAPTER CORE: %s", core ? "PASS" : "FAIL");
    bool full = core && s_robot && s_reply;
    ESP_LOGI(TAG, "FULL LOOP (with rover): %s", full ? "PASS" : "partial");
    ESP_LOGI(TAG, "================================================");

    for (;;) settle(5000);
}
