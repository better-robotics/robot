/*
 * WebSocket <-> TCP bridge — the piece that lets a *browser* reach the on-chip
 * broker (better-robotics/hub-mqtt#2). The Espressif Mosquitto port has no
 * WebSocket listener; browsers speak only MQTT-over-WebSocket. So this bridges:
 *
 *   browser  --ws://esp32:9001-->  [this bridge]  --tcp 127.0.0.1:1883-->  broker
 *
 * Pure byte-pump in both directions — MQTT carries its own length framing, so
 * the bridge never parses MQTT; it just moves bytes and preserves order.
 * WS->TCP happens in the httpd handler (httpd owns the WS socket); TCP->WS
 * happens in a per-client pump task via httpd_ws_send_frame_async.
 *
 * Lifetime: a fixed static slot pool (no malloc/free per client) — avoids the
 * task-vs-free_ctx use-after-free race a dynamic design invites. Validation-
 * grade, not hardened: idle-with-no-traffic WS closes are reclaimed on the
 * next broker->client byte (MQTT keepalive guarantees periodic traffic).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"   /* fleet identity chip: read the AP's own SSID */
#include "esp_log.h"
#include "lwip/sockets.h"
#include "wifi_portal.h"   /* share the board's always-on :80 rather than fighting for it */

#define WS_PORT      9001
#define BROKER_PORT  1883
#define MAX_BRIDGES  4
#define PUMP_BUF     1024

static const char *TAG = "ws-bridge";

typedef struct {
    volatile bool    in_use;
    volatile bool    closing;
    httpd_handle_t   server;
    int              client_fd;   /* browser WS socket (for async send) */
    int              broker_fd;   /* TCP to 127.0.0.1:1883 */
} bridge_t;

static bridge_t s_slots[MAX_BRIDGES];

static int connect_broker(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(BROKER_PORT),
        .sin_addr.s_addr = htonl(0x7f000001), /* 127.0.0.1 */
    };
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

/* TCP(broker) -> WS(browser) */
static void pump_task(void *arg)
{
    bridge_t *b = (bridge_t *)arg;
    uint8_t buf[PUMP_BUF];
    while (!b->closing) {
        int n = recv(b->broker_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break; /* broker closed, or shutdown() from free_cb woke us */
        }
        httpd_ws_frame_t f = {
            .final = true,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = buf,
            .len = n,
        };
        if (httpd_ws_send_frame_async(b->server, b->client_fd, &f) != ESP_OK) {
            break; /* browser gone */
        }
    }
    b->closing = true;
    if (b->broker_fd >= 0) {
        close(b->broker_fd);
        b->broker_fd = -1;
    }
    b->in_use = false; /* slot reclaimed */
    ESP_LOGI(TAG, "bridge closed (client_fd=%d)", b->client_fd);
    vTaskDelete(NULL);
}

/* Called by httpd when the WS session closes: wake the pump so it tears down. */
static void free_cb(void *ctx)
{
    bridge_t *b = (bridge_t *)ctx;
    if (!b) {
        return;
    }
    b->closing = true;
    if (b->broker_fd >= 0) {
        shutdown(b->broker_fd, SHUT_RDWR); /* unblock the pump's recv() */
    }
}

static bridge_t *slot_alloc(void)
{
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (!s_slots[i].in_use) {
            memset(&s_slots[i], 0, sizeof(bridge_t));
            s_slots[i].in_use = true;
            s_slots[i].broker_fd = -1;
            return &s_slots[i];
        }
    }
    return NULL;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    /* First call = the HTTP GET upgrade; httpd finishes the WS handshake.
     * Stand up this client's broker connection + pump here. */
    if (req->method == HTTP_GET) {
        bridge_t *b = slot_alloc();
        if (!b) {
            ESP_LOGW(TAG, "no free bridge slot (max %d clients)", MAX_BRIDGES);
            return ESP_FAIL;
        }
        b->server = req->handle;
        b->client_fd = httpd_req_to_sockfd(req);
        b->broker_fd = connect_broker();
        if (b->broker_fd < 0) {
            ESP_LOGE(TAG, "broker connect failed");
            b->in_use = false;
            return ESP_FAIL;
        }
        req->sess_ctx = b;
        req->free_ctx = free_cb;
        if (xTaskCreate(pump_task, "ws_pump", 4096, b, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "pump task create failed");
            close(b->broker_fd);
            b->in_use = false;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "bridge up (client_fd=%d -> broker_fd=%d)", b->client_fd, b->broker_fd);
        return ESP_OK;
    }

    /* Subsequent calls = WS frames from the browser. Two-call recv idiom. */
    bridge_t *b = (bridge_t *)req->sess_ctx;
    httpd_ws_frame_t ws = {0};
    ws.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws, 0); /* len only */
    if (ret != ESP_OK) {
        return ret;
    }
    if (ws.len == 0 || !b || b->broker_fd < 0) {
        return ESP_OK;
    }
    uint8_t *payload = malloc(ws.len);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    ws.payload = payload;
    ret = httpd_ws_recv_frame(req, &ws, ws.len);
    if (ret == ESP_OK &&
        (ws.type == HTTPD_WS_TYPE_BINARY || ws.type == HTTPD_WS_TYPE_TEXT)) {
        /* forward the whole frame to the broker (send-all) */
        size_t off = 0;
        while (off < ws.len) {
            int w = send(b->broker_fd, payload + off, ws.len - off, 0);
            if (w <= 0) {
                break;
            }
            off += w;
        }
    }
    free(payload);
    return ESP_OK;
}

/* The real dashboard.html (mqtt.js inlined), embedded so the ESP32 serves the
 * whole UI itself — no Pi, no separate web host. Browser loads it at "/"
 * (served mode → connects ws to "/" = the bridge above). Compiled in as a plain
 * byte array from the generated src/dashboard_html.c (tools/embed_dashboard.py,
 * driven by platformio.ini pre-build) rather than an objcopy .S embed, which
 * PlatformIO's build doesn't wire into the link. */
extern const unsigned char dashboard_html[];
extern const unsigned int  dashboard_html_len;

#include "ide_bundle.h"

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)dashboard_html, dashboard_html_len);
    return ESP_OK;
}

/* The block IDE (better-robotics/ide's ESP32 bundle — Blockly, no Monaco),
 * embedded like the dashboard but as a many-file table (src/ide_bundle.c,
 * generated by tools/embed_ide.py from web/ide/). Bodies are stored gzipped
 * and served with Content-Encoding: gzip straight from flash — the browser
 * inflates, the chip never does. One wildcard route (/ide/?*) instead of a
 * handler per file: the table has ~35 entries and the shared :80 server's
 * max_uri_handlers is deliberately tight (wifi_portal.c). */
static esp_err_t ide_handler(httpd_req_t *req)
{
    size_t n = strcspn(req->uri, "?");   /* compare without any query string */
    const char *want = req->uri;
    if (n == 4) {
        /* /ide -> /ide/ — the page's relative asset URLs (vendor/blockly/…)
         * only resolve under the trailing-slash form. */
        httpd_resp_set_status(req, "301 Moved Permanently");
        httpd_resp_set_hdr(req, "Location", "/ide/");
        return httpd_resp_send(req, NULL, 0);
    }
    if (n == 5) {
        want = "/ide/index.html";
        n = strlen(want);
    }
    for (unsigned int i = 0; i < ide_files_count; i++) {
        const ide_file_t *f = &ide_files[i];
        if (strlen(f->path) == n && strncmp(f->path, want, n) == 0) {
            httpd_resp_set_type(req, f->type);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_send(req, (const char *)f->data, f->len);
            return ESP_OK;
        }
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not in the IDE bundle");
}

/* The dashboard polls /fleet for the uplink pill + rover-setup locator. On the
 * Pi that's hubd; here the same httpd answers it so the real page works
 * unmodified. Uplink is honest, not hardcoded: an ESP hub/island with no STA
 * lease has no internet, and the dashboard's "none" pill tells the professor
 * exactly that (robots unaffected) instead of leaving them to debug the venue.
 * Portal detection needs an HTTP probe the Pi does — "full" here means only
 * "an uplink exists". */
static esp_err_t fleet_handler(httpd_req_t *req)
{
    const char *uplink = "none";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr)
        uplink = "full";
    /* ssid/host feed the dashboard's identity chip: which host serves this
     * page. Read from the live AP config, so a hub reports hub-XXXX and an
     * island board reports its own rover-XXXX — both truthful. */
    wifi_config_t apcfg = { 0 };
    const char *ssid = "";
    if (esp_wifi_get_config(WIFI_IF_AP, &apcfg) == ESP_OK)
        ssid = (const char *)apcfg.ap.ssid;
    /* The locator is this board's own broker at its live AP address — derived
     * like the SSID above, never hardcoded (CONTRACT § Discovery): a hub AP
     * sits on 192.168.4.1 but an island board's AP is relocated to .99.1. */
    char locator[48] = "";
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t apip;
    if (ap && esp_netif_get_ip_info(ap, &apip) == ESP_OK && apip.ip.addr)
        snprintf(locator, sizeof locator, "mqtt://" IPSTR ":1883", IP2STR(&apip.ip));
    char body[192];
    snprintf(body, sizeof body,
             "{\"uplink\":\"%s\",\"locator\":\"%s\","
             "\"ssid\":\"%s\",\"host\":\"esp32\"}", uplink, locator, ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* Two httpd instances, mirroring the Pi's port layout so the SAME dashboard.html
 * works on both: page on :80 (clean URL http://192.168.4.1/, like the Pi's
 * http://hub) and the WS bridge on :9001 (where Mosquitto's WS listener sits on
 * the Pi). The dashboard, served from :80, connects mqtt.js to ws://host:9001. */
void start_ws_mqtt_bridge(void)
{
    /* Instance 1 — the dashboard page + /fleet on :80. A normal board already runs
     * the Wi-Fi config panel on :80 (wifi_portal_start, from board_run), so we
     * register the dashboard onto THAT shared server and take over "/" from the
     * panel's setup-redirect. Only the tier-2 hub (no board_run, no panel) has no
     * :80 yet → it starts its own, preserving the original behavior. */
    httpd_handle_t page_srv = wifi_portal_httpd();
    if (page_srv) {
        /* Drop the panel's state-routing landing so the drive UI owns the home
         * page — the landing's dash=="/" reload lands exactly here. */
        httpd_unregister_uri_handler(page_srv, "/", HTTP_GET);
    } else {
        httpd_config_t page_cfg = HTTPD_DEFAULT_CONFIG();
        page_cfg.server_port = 80;
        page_cfg.ctrl_port = 32768;
        /* 7 (was 4): the IDE's first load fans out to ~10 asset requests and
         * browsers open up to 6 parallel connections per host. */
        page_cfg.max_open_sockets = 7;
        page_cfg.lru_purge_enable = true;
        /* The IDE's /ide/?* route needs wildcard matching; every exact URI
         * (no '*' in it) still matches exactly as before. The shared portal
         * server sets the same matcher (wifi_portal.c). */
        page_cfg.uri_match_fn = httpd_uri_match_wildcard;
        if (httpd_start(&page_srv, &page_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "page httpd (:80) failed to start");
            page_srv = NULL;
        }
    }
    if (page_srv) {
        httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = page_handler };
        httpd_uri_t fleet = { .uri = "/fleet", .method = HTTP_GET, .handler = fleet_handler };
        /* /ide, /ide/, and every asset under it — one wildcard slot. */
        httpd_uri_t ide = { .uri = "/ide/?*", .method = HTTP_GET, .handler = ide_handler };
        httpd_register_uri_handler(page_srv, &page);
        httpd_register_uri_handler(page_srv, &fleet);
        httpd_register_uri_handler(page_srv, &ide);
        ESP_LOGI(TAG, "dashboard at / and block IDE at /ide/ on :80 (this board's AP address)");
    }

    /* Instance 2 — the WS<->TCP bridge, on :9001. */
    httpd_config_t ws_cfg = HTTPD_DEFAULT_CONFIG();
    ws_cfg.server_port = WS_PORT;
    ws_cfg.ctrl_port = 32769;
    ws_cfg.max_open_sockets = MAX_BRIDGES + 2;
    ws_cfg.lru_purge_enable = true;
    httpd_handle_t ws_srv = NULL;
    if (httpd_start(&ws_srv, &ws_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "bridge httpd (:%d) failed to start", WS_PORT);
        return;
    }
    /* mqtt.js connects with no path (→ "/"); "/mqtt" too for clients using the
     * conventional path. "mqtt" subprotocol is what mqtt.js sends. */
    httpd_uri_t ws_root = { .uri = "/", .method = HTTP_GET, .handler = ws_handler,
                            .is_websocket = true, .supported_subprotocol = "mqtt" };
    httpd_uri_t ws_mqtt = { .uri = "/mqtt", .method = HTTP_GET, .handler = ws_handler,
                            .is_websocket = true, .supported_subprotocol = "mqtt" };
    httpd_register_uri_handler(ws_srv, &ws_root);
    httpd_register_uri_handler(ws_srv, &ws_mqtt);
    ESP_LOGI(TAG, "WS bridge on :%d (browser MQTT-over-WS -> on-chip broker)", WS_PORT);
}
