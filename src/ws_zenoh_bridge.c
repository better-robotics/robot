/*
 * WS-JSON adapter — the piece that lets a *browser* reach an ESP-hub's Zenoh
 * fabric (MQTT→Zenoh migration, hub#9). It is the sibling of ws_mqtt_bridge.c:
 * same esp_http_server WebSocket termination, same bounded static slot pool, the
 * same :80 page + :9001 WS split. What differs is the far side — a browser can't
 * speak native Zenoh (zenoh-pico's WS link is Emscripten-only; zenoh-ts needs a
 * Rust router), and there is no on-chip broker socket to pipe bytes to. So this
 * doesn't byte-pump: it PARSES a small JSON op protocol and maps each op onto the
 * hub's local zenoh-pico session by C API call —
 *
 *   {op:"sub", key} / {op:"unsub", key}   register/drop a per-client key filter
 *   {op:"pub", key, val}                   z_put (fleet/estop gated on auth)
 *   {op:"get", key, val, id}  -> reply     z_get; reply returns as {op:reply,id,val}
 *   {op:"auth", password}     -> {ok}      instructor gate (board_instructor_pass_ok)
 *
 *   hub -> client: {key, val}              a delivered subscription sample
 *
 * One session-wide subscriber on ** fans each sample out to the clients whose
 * filters match. The hub OWNS the fleet/estop latch: an authed estop pub updates
 * s_estop_latched and a queryable on fleet/estop answers a (re)joining rover's
 * join-time get — the retained-message latch of the MQTT era, as a query.
 *
 * Validation-grade static pool, exactly like ws_mqtt_bridge.c; the reap-on-STA-
 * leave hook mirrors it too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "zenoh-pico.h"
#include "roles.h"          /* board_uplink, board_instructor_pass_ok */
#include "wifi_portal.h"    /* share the board's always-on :80 */

#define WS_PORT      9001
#define MAX_CLIENTS  3       /* same LWIP-socket budget rationale as ws_mqtt_bridge.c's MAX_BRIDGES */
#define MAX_SUBS     6       /* per-client key filters — dashboard uses ~2 (a robots wildcard + fleet/estop) */
#define KEYLEN       48

static const char *TAG = "ws-zenoh";

typedef struct {
    volatile bool in_use;
    httpd_handle_t server;
    int  client_fd;
    bool authed;                     /* instructor — gates fleet/estop writes */
    char subs[MAX_SUBS][KEYLEN];
    int  nsubs;
} client_t;

static client_t s_clients[MAX_CLIENTS];
static z_owned_session_t *s_sess;    /* owned by hub_role; we borrow it per call */
static volatile bool s_estop_latched = false;   /* hub-owned fleet/estop latch */

/* ── Zenoh key-expression glob match (`*`=one chunk, `**`=zero+ chunks) ─────────
 * Enough of the grammar for the dashboard's filters (a robots wildcard,
 * fleet/estop, a per-robot imu channel). Segment-wise recursive. */
static bool ke_match(const char *pat, const char *key) {
    while (*pat && *key) {
        const char *pe = strchr(pat, '/');
        size_t plen = pe ? (size_t)(pe - pat) : strlen(pat);
        const char *ge = strchr(key, '/');
        size_t glen = ge ? (size_t)(ge - key) : strlen(key);
        if (plen == 2 && pat[0] == '*' && pat[1] == '*') {
            const char *pnext = pe ? pe + 1 : pat + plen;
            if (!*pnext) return true;
            for (const char *k = key;;) {
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
    return *pat == 0 && *key == 0;
}

/* ── outbound: one JSON frame to a browser client ──────────────────────────── */
static void ws_send_json(client_t *c, const char *json) {
    httpd_ws_frame_t f = {
        .final = true, .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json, .len = strlen(json),
    };
    if (httpd_ws_send_frame_async(c->server, c->client_fd, &f) != ESP_OK)
        c->in_use = false;   /* browser gone — reclaim */
}

/* ── zenoh sample -> every subscribed client (runs on the read task) ───────── */
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
            for (int j = 0; j < c->nsubs; j++)
                if (ke_match(c->subs[j], key)) { ws_send_json(c, frame); break; }
        }
    }
    z_drop(z_move(val));
}

/* ── get reply -> the requesting client ────────────────────────────────────── */
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
static void on_reply_drop(void *ctx) { free(ctx); }

/* ── the hub-owned fleet/estop latch: a queryable answering current state ───── */
static void on_estop_query(z_loaned_query_t *query, void *ctx) {
    (void)ctx;
    char body[32];
    snprintf(body, sizeof body, "{\"engaged\":%s}", s_estop_latched ? "true" : "false");
    z_owned_bytes_t reply;
    z_bytes_copy_from_str(&reply, body);
    z_query_reply(query, z_query_keyexpr(query), z_move(reply), NULL);
}

/* Update the latch from an estop payload the hub is about to publish. Empty or
 * unparseable = engaged (fail toward stopped, same bias as the rover). */
static void latch_from_payload(const char *json, int len) {
    bool engaged = true;
    if (len == 0) {
        engaged = false;
    } else {
        cJSON *r = cJSON_ParseWithLength(json, len);
        if (r) { engaged = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "engaged")); cJSON_Delete(r); }
    }
    s_estop_latched = engaged;
}

/* ── op handlers ───────────────────────────────────────────────────────────── */
static void do_pub(const char *key, cJSON *val) {
    char *vs = cJSON_PrintUnformatted(val);
    if (!vs) return;
    z_view_keyexpr_t ke;
    if (z_view_keyexpr_from_str(&ke, key) == 0) {
        z_owned_bytes_t p;
        z_bytes_copy_from_str(&p, vs);
        z_put(z_loan(*s_sess), z_loan(ke), z_move(p), NULL);
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
    if (vs) { z_owned_bytes_t p; z_bytes_copy_from_str(&p, vs); opts.payload = z_move(p); }
    z_get(z_loan(*s_sess), z_loan(ke), "", z_move(cb), &opts);
    free(vs);
}

static void handle_op(client_t *c, const char *text, size_t len) {
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *op = cJSON_GetObjectItem(root, "op");
    const cJSON *key = cJSON_GetObjectItem(root, "key");
    if (cJSON_IsString(op)) {
        const char *o = op->valuestring;
        if (!strcmp(o, "sub") && cJSON_IsString(key)) {
            if (c->nsubs < MAX_SUBS) { snprintf(c->subs[c->nsubs], KEYLEN, "%s", key->valuestring); c->nsubs++; }
        } else if (!strcmp(o, "unsub") && cJSON_IsString(key)) {
            for (int j = 0; j < c->nsubs; j++)
                if (!strcmp(c->subs[j], key->valuestring)) { memmove(c->subs[j], c->subs[c->nsubs - 1], KEYLEN); c->nsubs--; break; }
        } else if (!strcmp(o, "pub") && cJSON_IsString(key)) {
            cJSON *val = cJSON_GetObjectItem(root, "val");
            bool is_estop = !strcmp(key->valuestring, "fleet/estop");
            if (is_estop && !c->authed) {
                ws_send_json(c, "{\"op\":\"error\",\"reason\":\"estop requires instructor auth\"}");
            } else if (val) {
                if (is_estop) {
                    /* Update the hub-owned latch BEFORE the put — a joining rover's
                     * query and the live subscribers must agree, and a local put
                     * never loops back to our own subscriber to do it for us. */
                    char *vs = cJSON_PrintUnformatted(val);
                    if (vs) { latch_from_payload(vs, strlen(vs)); free(vs); }
                }
                do_pub(key->valuestring, val);
            }
        } else if (!strcmp(o, "get") && cJSON_IsString(key)) {
            cJSON *val = cJSON_GetObjectItem(root, "val");
            const cJSON *jid = cJSON_GetObjectItem(root, "id");
            do_get(c, key->valuestring, val, cJSON_IsString(jid) ? jid->valuestring : "");
        } else if (!strcmp(o, "auth")) {
            const cJSON *pw = cJSON_GetObjectItem(root, "password");
            c->authed = cJSON_IsString(pw) && board_instructor_pass_ok(pw->valuestring);
            char r[48];
            snprintf(r, sizeof r, "{\"op\":\"auth\",\"ok\":%s}", c->authed ? "true" : "false");
            ws_send_json(c, r);
            ESP_LOGI(TAG, "auth -> %s", c->authed ? "ok" : "reject");
        }
    }
    cJSON_Delete(root);
}

/* ── httpd WS termination (mirrors ws_mqtt_bridge.c) ────────────────────────── */
static client_t *slot_alloc(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!s_clients[i].in_use) { memset(&s_clients[i], 0, sizeof(client_t)); s_clients[i].in_use = true; return &s_clients[i]; }
    return NULL;
}
static void ws_free_cb(void *ctx) { client_t *c = ctx; if (c) c->in_use = false; }

/* Same STA-leave reap as ws_mqtt_bridge.c: a browser that vanishes leaves a slot
 * held, and httpd's own detection isn't prompt. hub_role calls this on
 * WIFI_EVENT_AP_STADISCONNECTED. Closing all is a self-healing blip (the page
 * reconnects) against a 3-slot budget. */
void ws_zenoh_reap_all(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (!c->in_use) continue;
        ESP_LOGW(TAG, "station left — force-closing ws client (fd=%d)", c->client_fd);
        close(c->client_fd);
        c->in_use = false;
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        client_t *c = slot_alloc();
        if (!c) { ESP_LOGW(TAG, "no free ws client slot (max %d)", MAX_CLIENTS); return ESP_FAIL; }
        c->server = req->handle;
        c->client_fd = httpd_req_to_sockfd(req);
        int ka = 1, idle = 5, intvl = 5, cnt = 2;   /* reap vanished browsers ~15 s */
        setsockopt(c->client_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
        setsockopt(c->client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(c->client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        setsockopt(c->client_fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
        req->sess_ctx = c;
        req->free_ctx = ws_free_cb;
        ESP_LOGI(TAG, "ws client up (fd=%d)", c->client_fd);
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

/* ── the :80 page + /fleet, reused from the dashboard the MQTT bridge served ──
 * Identical bytes as ws_mqtt_bridge.c serves; the only wire difference is the
 * /fleet locator, which now advertises the Zenoh endpoint. */
extern const unsigned char dashboard_html[];
extern const unsigned int  dashboard_html_len;
extern const unsigned char ide_shell_html[];
extern const unsigned int  ide_shell_html_len;

static esp_err_t page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)dashboard_html, dashboard_html_len);
    return ESP_OK;
}
static esp_err_t ide_shell_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)ide_shell_html, ide_shell_html_len);
    return ESP_OK;
}
static esp_err_t ide_redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/ide/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
static esp_err_t fleet_handler(httpd_req_t *req) {
    static const char *up_str[] = { "none", "portal", "full" };
    const char *uplink = up_str[board_uplink()];
    wifi_config_t apcfg = { 0 };
    const char *ssid = "";
    if (esp_wifi_get_config(WIFI_IF_AP, &apcfg) == ESP_OK) ssid = (const char *)apcfg.ap.ssid;
    /* Locator now advertises Zenoh's TCP endpoint, not mqtt://:1883. */
    char locator[48] = "";
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t apip;
    if (ap && esp_netif_get_ip_info(ap, &apip) == ESP_OK && apip.ip.addr)
        snprintf(locator, sizeof locator, "tcp/" IPSTR ":7447", IP2STR(&apip.ip));
    char body[160];
    snprintf(body, sizeof body,
             "{\"uplink\":\"%s\",\"locator\":\"%s\",\"ssid\":\"%s\",\"host\":\"esp32\"}",
             uplink, locator, ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* ── start_ws_zenoh_bridge: stand up the adapter over an already-open session ──
 * `session` is the hub's peer-listen session (hub_role owns it). Declares the **
 * fan-out subscriber and the fleet/estop queryable on it, then the :80 page and
 * :9001 WS servers — the same two-httpd layout the MQTT bridge used. */
void start_ws_zenoh_bridge(z_owned_session_t *session) {
    s_sess = session;

    z_owned_closure_sample_t scb;
    z_closure(&scb, on_sample, NULL, NULL);
    static z_owned_subscriber_t s_fanout;
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, "**");
    if (z_declare_subscriber(z_loan(*s_sess), &s_fanout, z_loan(ke), z_move(scb), NULL) < 0)
        ESP_LOGE(TAG, "declare ** fan-out subscriber failed");

    z_owned_closure_query_t qcb;
    z_closure(&qcb, on_estop_query, NULL, NULL);
    static z_owned_queryable_t s_estop_qab;
    z_view_keyexpr_t qke;
    z_view_keyexpr_from_str_unchecked(&qke, "fleet/estop");
    if (z_declare_queryable(z_loan(*s_sess), &s_estop_qab, z_loan(qke), z_move(qcb), NULL) < 0)
        ESP_LOGE(TAG, "declare fleet/estop queryable failed");

    /* Page + /fleet on the board's shared :80 (like ws_mqtt_bridge.c). */
    httpd_handle_t page_srv = wifi_portal_httpd();
    if (page_srv) {
        httpd_unregister_uri_handler(page_srv, "/", HTTP_GET);
    } else {
        httpd_config_t pc = HTTPD_DEFAULT_CONFIG();
        pc.server_port = 80; pc.ctrl_port = 32768; pc.max_open_sockets = 3; pc.lru_purge_enable = true;
        if (httpd_start(&page_srv, &pc) != ESP_OK) { ESP_LOGE(TAG, "page httpd (:80) failed"); page_srv = NULL; }
    }
    if (page_srv) {
        httpd_uri_t page  = { .uri = "/",      .method = HTTP_GET, .handler = page_handler };
        httpd_uri_t fleet = { .uri = "/fleet", .method = HTTP_GET, .handler = fleet_handler };
        httpd_uri_t ide   = { .uri = "/ide/",  .method = HTTP_GET, .handler = ide_shell_handler };
        httpd_uri_t ide_r = { .uri = "/ide",   .method = HTTP_GET, .handler = ide_redirect_handler };
        httpd_register_uri_handler(page_srv, &page);
        httpd_register_uri_handler(page_srv, &fleet);
        httpd_register_uri_handler(page_srv, &ide);
        httpd_register_uri_handler(page_srv, &ide_r);
    }

    /* WS adapter on :9001 (where the MQTT bridge's WS listener sat). */
    httpd_config_t wc = HTTPD_DEFAULT_CONFIG();
    wc.server_port = WS_PORT; wc.ctrl_port = 32769; wc.max_open_sockets = MAX_CLIENTS + 1; wc.lru_purge_enable = true;
    httpd_handle_t ws_srv = NULL;
    if (httpd_start(&ws_srv, &wc) != ESP_OK) { ESP_LOGE(TAG, "ws httpd (:%d) failed", WS_PORT); return; }
    httpd_uri_t ws_root = { .uri = "/",     .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_uri_t ws_alt  = { .uri = "/mqtt", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_register_uri_handler(ws_srv, &ws_root);
    httpd_register_uri_handler(ws_srv, &ws_alt);
    ESP_LOGI(TAG, "WS-JSON adapter on :%d, dashboard on :80", WS_PORT);
}
