// Stage 4 spike — the ESP32-C3 as a zenoh-pico HUB *with the browser edge*.
//
// Extends the proven Stage-2 hub (SoftAP + zenoh-pico peer-listen) with the
// WS-JSON adapter the browser dashboard needs: a WebSocket on :9001 that maps
// small JSON ops {op:pub|sub|get|auth} onto the hub's LOCAL zenoh-pico session.
// This is the sibling of robot/src/ws_mqtt_bridge.c — same httpd/WS termination
// and bounded slot pool — but the byte-pump-to-broker is replaced by op parsing,
// because Zenoh has no on-chip broker socket to pipe to; the "broker" is this
// firmware's own zenoh session, reached by C API calls.
//
// Proves the last open architectural gate in hub/zenoh-migration.md:
//   browser --ws://192.168.4.1:9001--> [adapter] --zenoh--> rover peer   (and back)
//
// A tiny page on :80 lets a browser drive it; a JSON op log makes the round-trip
// visible on the serial monitor.

#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "zenoh-pico.h"

#define AP_CHANNEL 6
#define LED_GPIO GPIO_NUM_8   // C3 SuperMini onboard LED (active low)
#define WS_PORT 9001
#define MAX_CLIENTS 3
#define MAX_SUBS 6            // per-client subscribed key-exprs
#define KEYLEN 48
#define INSTRUCTOR_PASS "letmestop"  // spike only — real value is provisioned

static const char *TAG = "zwshub";

// ---- WS-JSON adapter client slots ------------------------------------------
typedef struct {
    volatile bool in_use;
    httpd_handle_t server;
    int client_fd;
    bool authed;                 // instructor auth (gates fleet/estop writes)
    char subs[MAX_SUBS][KEYLEN]; // key-exprs this client wants delivered
    int nsubs;
} client_t;

static client_t s_clients[MAX_CLIENTS];
static z_owned_session_t s_session;

// ---- Zenoh key-expression glob match (`*` = one chunk, `**` = zero+ chunks) --
// Enough of the ke grammar for the dashboard's real subscriptions
// (`robots/**`, `fleet/estop`, `robots/<id>/imu`). Segment-wise recursive.
static bool ke_match(const char *pat, const char *key) {
    while (*pat && *key) {
        const char *pe = strchr(pat, '/');
        size_t plen = pe ? (size_t)(pe - pat) : strlen(pat);
        const char *ge = strchr(key, '/');
        size_t glen = ge ? (size_t)(ge - key) : strlen(key);

        if (plen == 2 && pat[0] == '*' && pat[1] == '*') {
            // `**` matches zero or more chunks: try consuming 0..n here.
            const char *pnext = pe ? pe + 1 : pat + plen;  // pattern after **
            if (!*pnext) return true;                      // trailing ** => all
            for (const char *k = key;; ) {
                if (ke_match(pnext, k)) return true;
                const char *nx = strchr(k, '/');
                if (!nx) return false;
                k = nx + 1;
            }
        }
        bool seg_ok = (plen == 1 && pat[0] == '*') ||
                      (plen == glen && strncmp(pat, key, plen) == 0);
        if (!seg_ok) return false;
        pat = pe ? pe + 1 : pat + plen;
        key = ge ? ge + 1 : key + glen;
    }
    // both must be exhausted (a trailing `/**` is handled above)
    return *pat == 0 && *key == 0;
}

// ---- outbound: send one JSON frame to a browser client ----------------------
static void ws_send_json(client_t *c, const char *json) {
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    if (httpd_ws_send_frame_async(c->server, c->client_fd, &f) != ESP_OK) {
        c->in_use = false;  // browser gone — reclaim the slot
    }
}

// ---- zenoh sample -> every subscribed client --------------------------------
// Runs in the zenoh read task; fans a delivered sample out to matching clients.
static void on_sample(z_loaned_sample_t *sample, void *arg) {
    (void)arg;
    z_view_string_t ks;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &ks);
    char key[KEYLEN];
    size_t kn = z_string_len(z_view_string_loan(&ks));
    if (kn >= KEYLEN) kn = KEYLEN - 1;
    memcpy(key, z_string_data(z_view_string_loan(&ks)), kn);
    key[kn] = 0;

    z_owned_string_t val;
    z_bytes_to_string(z_sample_payload(sample), &val);
    const char *vd = z_string_data(z_string_loan(&val));
    size_t vn = z_string_len(z_string_loan(&val));

    char frame[512];
    int fl = snprintf(frame, sizeof frame, "{\"key\":\"%s\",\"val\":%.*s}", key, (int)vn, vd);
    if (fl > 0 && fl < (int)sizeof frame) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &s_clients[i];
            if (!c->in_use) continue;
            for (int j = 0; j < c->nsubs; j++) {
                if (ke_match(c->subs[j], key)) { ws_send_json(c, frame); break; }
            }
        }
    }
    z_drop(z_move(val));
}

// ---- get reply -> the requesting client -------------------------------------
typedef struct { client_t *c; char id[24]; } getctx_t;

static void on_reply(z_loaned_reply_t *reply, void *ctx) {
    getctx_t *g = (getctx_t *)ctx;
    if (!z_reply_is_ok(reply)) return;
    const z_loaned_sample_t *sample = z_reply_ok(reply);
    z_owned_string_t val;
    z_bytes_to_string(z_sample_payload(sample), &val);
    char frame[512];
    int fl = snprintf(frame, sizeof frame, "{\"op\":\"reply\",\"id\":\"%s\",\"val\":%.*s}",
                      g->id, (int)z_string_len(z_string_loan(&val)), z_string_data(z_string_loan(&val)));
    if (fl > 0 && fl < (int)sizeof frame && g->c->in_use) ws_send_json(g->c, frame);
    z_drop(z_move(val));
}
static void on_reply_drop(void *ctx) { free(ctx); }  // one query, one ctx

// ---- op handlers ------------------------------------------------------------
static void do_pub(const char *key, cJSON *val) {
    char *vs = cJSON_PrintUnformatted(val);
    if (!vs) return;
    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, key) == 0) {
        z_owned_bytes_t p;
        z_bytes_copy_from_str(&p, vs);
        z_put(z_loan(s_session), z_loan(ke), z_move(p), NULL);
        ESP_LOGI(TAG, "PUB %s = %s", key, vs);
    }
    free(vs);
}

static void do_get(client_t *c, const char *key, cJSON *val, const char *id) {
    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, key) != 0) return;
    getctx_t *g = calloc(1, sizeof *g);
    if (!g) return;
    g->c = c;
    snprintf(g->id, sizeof g->id, "%s", id ? id : "");
    z_owned_closure_reply_t cb;
    z_closure(&cb, on_reply, on_reply_drop, g);
    z_get_options_t opts;
    z_get_options_default(&opts);
    char *vs = val ? cJSON_PrintUnformatted(val) : NULL;
    if (vs) {
        z_owned_bytes_t p;
        z_bytes_copy_from_str(&p, vs);
        opts.payload = z_move(p);
    }
    z_get(z_loan(s_session), z_loan(ke), "", z_move(cb), &opts);
    ESP_LOGI(TAG, "GET %s (id=%s)", key, id ? id : "");
    free(vs);
}

// Parse one browser JSON op and act on the local zenoh session.
static void handle_op(client_t *c, const char *text, size_t len) {
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *op = cJSON_GetObjectItem(root, "op");
    const cJSON *key = cJSON_GetObjectItem(root, "key");
    if (cJSON_IsString(op)) {
        const char *o = op->valuestring;
        if (!strcmp(o, "sub") && cJSON_IsString(key)) {
            if (c->nsubs < MAX_SUBS) {
                snprintf(c->subs[c->nsubs], KEYLEN, "%s", key->valuestring);
                c->nsubs++;
                ESP_LOGI(TAG, "SUB %s (client_fd=%d)", key->valuestring, c->client_fd);
            }
        } else if (!strcmp(o, "unsub") && cJSON_IsString(key)) {
            for (int j = 0; j < c->nsubs; j++) {
                if (!strcmp(c->subs[j], key->valuestring)) {
                    memmove(c->subs[j], c->subs[c->nsubs - 1], KEYLEN);
                    c->nsubs--;
                    break;
                }
            }
        } else if (!strcmp(o, "pub") && cJSON_IsString(key)) {
            cJSON *val = cJSON_GetObjectItem(root, "val");
            // instructor gate: e-stop writes require prior auth
            if (!strcmp(key->valuestring, "fleet/estop") && !c->authed) {
                ws_send_json(c, "{\"op\":\"error\",\"reason\":\"estop requires instructor auth\"}");
            } else if (val) {
                do_pub(key->valuestring, val);
            }
        } else if (!strcmp(o, "get") && cJSON_IsString(key)) {
            cJSON *val = cJSON_GetObjectItem(root, "val");
            const cJSON *jid = cJSON_GetObjectItem(root, "id");
            do_get(c, key->valuestring, val, cJSON_IsString(jid) ? jid->valuestring : "");
        } else if (!strcmp(o, "auth")) {
            const cJSON *pw = cJSON_GetObjectItem(root, "password");
            c->authed = cJSON_IsString(pw) && !strcmp(pw->valuestring, INSTRUCTOR_PASS);
            char r[48];
            snprintf(r, sizeof r, "{\"op\":\"auth\",\"ok\":%s}", c->authed ? "true" : "false");
            ws_send_json(c, r);
            ESP_LOGI(TAG, "AUTH -> %s", c->authed ? "ok" : "reject");
        }
    }
    cJSON_Delete(root);
}

// ---- httpd WS handler (mirrors ws_mqtt_bridge.c termination) -----------------
static client_t *slot_alloc(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            memset(&s_clients[i], 0, sizeof(client_t));
            s_clients[i].in_use = true;
            return &s_clients[i];
        }
    }
    return NULL;
}

static void ws_free_cb(void *ctx) {
    client_t *c = (client_t *)ctx;
    if (c) c->in_use = false;  // no subscribers to undeclare — the session sub is shared
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        client_t *c = slot_alloc();
        if (!c) { ESP_LOGW(TAG, "no free client slot (max %d)", MAX_CLIENTS); return ESP_FAIL; }
        c->server = req->handle;
        c->client_fd = httpd_req_to_sockfd(req);
        int ka = 1, idle = 5, intvl = 5, cnt = 2;  // reap vanished browsers (~15s)
        setsockopt(c->client_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
        setsockopt(c->client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(c->client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        setsockopt(c->client_fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
        req->sess_ctx = c;
        req->free_ctx = ws_free_cb;
        ESP_LOGI(TAG, "adapter client up (client_fd=%d)", c->client_fd);
        return ESP_OK;
    }
    client_t *c = (client_t *)req->sess_ctx;
    httpd_ws_frame_t ws = {0};
    ws.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws, 0);
    if (ret != ESP_OK || ws.len == 0 || !c) return ret;
    uint8_t *payload = malloc(ws.len + 1);
    if (!payload) return ESP_ERR_NO_MEM;
    ws.payload = payload;
    ret = httpd_ws_recv_frame(req, &ws, ws.len);
    if (ret == ESP_OK && (ws.type == HTTPD_WS_TYPE_TEXT || ws.type == HTTPD_WS_TYPE_BINARY)) {
        payload[ws.len] = 0;
        handle_op(c, (const char *)payload, ws.len);
    }
    free(payload);
    return ESP_OK;
}

// A tiny page so a browser can drive the adapter (served on :80).
static const char TEST_PAGE[] =
    "<!doctype html><meta charset=utf-8><title>zwshub</title>"
    "<h3>WS-JSON adapter probe</h3><pre id=o></pre><script>"
    "var o=document.getElementById('o'),w=new WebSocket('ws://'+location.hostname+':9001');"
    "function log(m){o.textContent+=m+'\\n'}"
    "w.onopen=function(){log('open');w.send(JSON.stringify({op:'sub',key:'robots/**'}));"
    "w.send(JSON.stringify({op:'sub',key:'hub/heartbeat'}));};"
    "w.onmessage=function(e){log('RX '+e.data)};"
    "window.drive=function(v){w.send(JSON.stringify({op:'pub',key:'robots/rover-z/pwm',"
    "val:{left_motor:v,right_motor:v,duration_ms:3000}}))};"
    "window.led=function(){w.send(JSON.stringify({op:'get',id:'g1',key:'robots/rover-z/led',"
    "val:{on:true,green:255}}))};</script>"
    "<button onclick=drive(200)>drive</button><button onclick=drive(0)>stop</button>"
    "<button onclick=led()>set_led</button>";

static esp_err_t page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, TEST_PAGE, sizeof TEST_PAGE - 1);
    return ESP_OK;
}

static void start_httpd(void) {
    httpd_handle_t page_srv = NULL;
    httpd_config_t pc = HTTPD_DEFAULT_CONFIG();
    pc.server_port = 80;
    pc.ctrl_port = 32768;
    pc.max_open_sockets = 3;
    pc.lru_purge_enable = true;
    if (httpd_start(&page_srv, &pc) == ESP_OK) {
        httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = page_handler };
        httpd_register_uri_handler(page_srv, &page);
    }
    httpd_handle_t ws_srv = NULL;
    httpd_config_t wc = HTTPD_DEFAULT_CONFIG();
    wc.server_port = WS_PORT;
    wc.ctrl_port = 32769;
    wc.max_open_sockets = MAX_CLIENTS + 1;
    wc.lru_purge_enable = true;
    if (httpd_start(&ws_srv, &wc) == ESP_OK) {
        httpd_uri_t ws = { .uri = "/", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
        httpd_register_uri_handler(ws_srv, &ws);
        ESP_LOGI(TAG, "WS-JSON adapter on :%d, test page on :80", WS_PORT);
    }
}

// ---- Wi-Fi SoftAP -----------------------------------------------------------
static void softap_start(char *ssid_out) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    sprintf(ssid_out, "hub-%02x%02x", mac[4], mac[5]);
    wifi_config_t ap = {.ap = {.channel = AP_CHANNEL, .max_connection = 4, .authmode = WIFI_AUTH_OPEN}};
    size_t n = strlen(ssid_out);
    memcpy(ap.ap.ssid, ssid_out, n);
    ap.ap.ssid_len = n;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // no dozing — this is the hub
    ESP_LOGI(TAG, "SoftAP up: SSID=%s gateway=192.168.4.1", ssid_out);
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
    gpio_set_level(LED_GPIO, 1);

    char ssid[16];
    softap_start(ssid);

    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "peer");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, "tcp/0.0.0.0:7447");

    ESP_LOGI(TAG, "opening zenoh peer (listen tcp/0.0.0.0:7447)...");
    z_open_options_t oo;
    z_open_options_default(&oo);
    oo.auto_start_read_task = false;
    oo.auto_start_lease_task = false;
    if (z_open(&s_session, z_move(config), &oo) < 0) {
        ESP_LOGE(TAG, "z_open failed");
        esp_restart();
    }
    if (zp_start_read_task(z_loan_mut(s_session), NULL) < 0 ||
        zp_start_lease_task(z_loan_mut(s_session), NULL) < 0) {
        ESP_LOGE(TAG, "read/lease task start failed");
        esp_restart();
    }
    ESP_LOGI(TAG, "zenoh peer open + tasks started");

    // One session-wide subscriber on ** fans every sample out to matching clients.
    z_owned_closure_sample_t cb;
    z_closure(&cb, on_sample, NULL, NULL);
    z_owned_subscriber_t sub;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, "**");
    if (z_declare_subscriber(z_loan(s_session), &sub, z_loan(ke), z_move(cb), NULL) < 0) {
        ESP_LOGE(TAG, "declare subscriber ** failed");
        esp_restart();
    }

    start_httpd();
    ESP_LOGI(TAG, "hub ready — browser drives it at http://192.168.4.1/");

    // Hub heartbeat: lets a browser see sub fan-out even with no rover present.
    z_view_keyexpr_t hbke;
    z_view_keyexpr_from_str_unchecked(&hbke, "hub/heartbeat");
    for (uint32_t i = 0;; i++) {
        wifi_sta_list_t sta;
        esp_wifi_ap_get_sta_list(&sta);
        gpio_set_level(LED_GPIO, (i & 1) ? 1 : 0);
        if (i % 2 == 0) {
            char buf[96];
            snprintf(buf, sizeof buf, "{\"beat\":%u,\"stations\":%d,\"heap\":%u}",
                     (unsigned)i, sta.num, (unsigned)esp_get_free_heap_size());
            z_owned_bytes_t p;
            z_bytes_copy_from_str(&p, buf);
            z_put(z_loan(s_session), z_loan(hbke), z_move(p), NULL);
        }
        int nclients = 0;
        for (int k = 0; k < MAX_CLIENTS; k++) if (s_clients[k].in_use) nclients++;
        if (i % 10 == 0)
            ESP_LOGI(TAG, "alive: %d station(s), %d ws client(s), heap %u",
                     sta.num, nclients, (unsigned)esp_get_free_heap_size());
        sleep(1);
    }
}
