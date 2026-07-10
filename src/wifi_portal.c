/*
 * wifi_portal.c — the per-board Wi-Fi config panel (robot#2 / #17).
 *
 * One always-on httpd on the board's :80, started by board_run right after the AP
 * comes up. It is what cashes in always-APSTA's promise: the board keeps its own
 * open rover-<id> AP up forever, so a student joins it, browses rover.local, and
 * sets home Wi-Fi WITHOUT the board ever switching radio mode. Works in every mode
 * — a classroom rover runs no broker or dashboard of its own, so without this it
 * would serve nothing at rover.local.
 *
 * The scan itself lives in hub_role.c (board_wifi_scan) because that file owns the
 * radio and the s_want_connect reconnect gate; this file is only presentation +
 * NVS + the config-apply reboot.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "roles.h"          /* board_ap_t, board_wifi_scan */
#include "rover_config.h"   /* rover_config_set_wifi, rover_config_load */
#include "wifi_portal.h"

static const char *TAG = "wifi-portal";
static httpd_handle_t s_http = NULL;

#define SCAN_MAX 24

/* The config page. Self-contained (no external CSS/JS/fonts — it is served by the
 * board with no internet): Scan lists visible networks by name/signal; tapping one
 * fills the SSID; Save posts and the board restarts onto the chosen network. */
static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>Rover Wi-Fi</title><style>"
"body{font:16px system-ui,sans-serif;max-width:26rem;margin:2rem auto;padding:0 1rem;"
"background:#111;color:#eee}h1{font-size:1.3rem}"
"input,button{font:inherit;width:100%;box-sizing:border-box;padding:.6rem;margin:.3rem 0;"
"border-radius:.5rem;border:1px solid #444;background:#1c1c1c;color:#eee}"
"button{background:#2563eb;border:0;cursor:pointer}button:active{background:#1d4ed8}"
"#nets button{background:#1c1c1c;border:1px solid #333;text-align:left;display:flex;justify-content:space-between}"
".s{color:#888;font-size:.85rem}#msg{margin-top:1rem}"
"</style></head><body>"
"<h1>Set the rover&rsquo;s Wi-Fi</h1>"
"<p class=s>Pick your home network, enter its password, and the rover restarts to join it.</p>"
"<button onclick=scan()>Scan for networks</button>"
"<div id=nets></div>"
"<form onsubmit=\"return save(event)\">"
"<input id=ssid name=ssid placeholder=\"Network name\" autocapitalize=off autocorrect=off required>"
"<input id=pass name=pass type=password placeholder=\"Password (blank if open)\">"
"<button type=submit>Save &amp; restart</button></form>"
"<div id=msg></div>"
"<script>"
"async function scan(){let d=document.getElementById('nets');d.innerHTML='<p class=s>scanning\\u2026</p>';"
"try{let r=await fetch('/wifi/scan');let a=await r.json();"
"if(!a.length){d.innerHTML='<p class=s>no networks found \\u2014 try again</p>';return}"
"d.innerHTML='';a.forEach(n=>{let b=document.createElement('button');b.type='button';"
/* textContent, NOT innerHTML: an SSID is attacker-controllable (any nearby AP can
 * name itself with markup), so it must never be parsed as HTML. */
"let s1=document.createElement('span');s1.textContent=n.ssid+(n.open?'':' \\uD83D\\uDD12');"
"let s2=document.createElement('span');s2.className='s';s2.textContent=n.rssi+' dBm';"
"b.append(s1,s2);"
"b.onclick=()=>{document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus()};"
"d.appendChild(b)})}catch(e){d.innerHTML='<p class=s>scan failed \\u2014 try again</p>'}}"
"async function save(e){e.preventDefault();let m=document.getElementById('msg');m.textContent='saving\\u2026';"
"let f=new URLSearchParams(new FormData(e.target));"
"try{await fetch('/wifi/save',{method:'POST',body:f});"
/* textContent again: the ssid value is device-derived, so echo it as text, never HTML. */
"let nm=document.createElement('b');nm.textContent=document.getElementById('ssid').value;"
"m.textContent='Saved. The rover is restarting to join ';m.appendChild(nm);"
"m.append('. Reconnect your device to that network, then open rover.local.')}"
"catch(x){m.textContent='Saved. The rover is restarting.'}return false}"
"</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

/* Bare rover.local → the setup page. When the board islands, start_ws_mqtt_bridge
 * unregisters this and serves the drive dashboard at / instead. */
static esp_err_t root_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/wifi");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    board_ap_t *aps = malloc(SCAN_MAX * sizeof *aps);
    if (!aps) { httpd_resp_send_500(req); return ESP_FAIL; }
    int n = board_wifi_scan(aps, SCAN_MAX);

    /* Each row ~ {"ssid":"<=32","rssi":-100,"open":false}, — budget generously. */
    size_t cap = 32 + (size_t)n * 96;
    char *json = malloc(cap);
    if (!json) { free(aps); httpd_resp_send_500(req); return ESP_FAIL; }
    size_t off = 0;
    off += snprintf(json + off, cap - off, "[");
    for (int i = 0; i < n; i++) {
        /* ssid is from a scan (device-supplied, attacker-controllable). Escape " and
         * \\ and DROP control bytes (<0x20): raw control chars are invalid JSON (would
         * break JSON.parse) and defense-in-depth against injection — the page renders
         * SSIDs with textContent, so legal high/UTF-8 bytes pass through safely. */
        char esc[65]; size_t e = 0;
        for (const unsigned char *p = (const unsigned char *)aps[i].ssid; *p && e < sizeof esc - 2; p++) {
            if (*p < 0x20) continue;
            if (*p == '"' || *p == '\\') esc[e++] = '\\';
            esc[e++] = (char)*p;
        }
        esc[e] = 0;
        off += snprintf(json + off, cap - off, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                        i ? "," : "", esc, aps[i].rssi, aps[i].open ? "true" : "false");
    }
    snprintf(json + off, cap - off, "]");

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json); free(aps);
    return r;
}

/* Decode one application/x-www-form-urlencoded field ("+"→space, %XX→byte) from
 * body into out. Returns true if `key` was present. */
static bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t o = 0;
            for (const char *q = v; q < end && o < outlen - 1; q++) {
                if (*q == '+') { out[o++] = ' '; }
                else if (*q == '%' && q + 2 < end + 1 && q[1] && q[2]) {
                    char h[3] = { q[1], q[2], 0 };
                    out[o++] = (char)strtol(h, NULL, 16);
                    q += 2;
                } else { out[o++] = *q; }
            }
            out[o] = 0;
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    out[0] = 0;
    return false;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the HTTP response flush + socket close */
    ESP_LOGW(TAG, "config-apply restart — joining the newly-saved network");
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[256];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int r = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        total += r;
    }
    body[total] = 0;

    char ssid[33], pass[65];
    if (!form_field(body, "ssid", ssid, sizeof ssid) || !ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing ssid");
    }
    form_field(body, "pass", pass, sizeof pass);   /* absent → open network */

    esp_err_t e = rover_config_set_wifi(ssid, pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rover_config_set_wifi failed: %s", esp_err_to_name(e));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved home Wi-Fi '%s' — restarting to join it", ssid);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "saved");
    /* A config-apply reboot: the loop reads NVS only at the top of a pass but sits
     * blocked driving, so a restart is the simplest correct way to re-dial the new
     * network. NOT the deleted mode-switch reboot — always-APSTA comes right back up. */
    xTaskCreate(reboot_task, "cfg-reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void wifi_portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.max_open_sockets = 7;
    cfg.max_uri_handlers = 12;   /* /wifi{,/scan,/save} + / here; +/fleet & dashboard / later */
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "config panel httpd (:80) failed to start");
        s_http = NULL;
        return;
    }
    httpd_uri_t u_page  = { .uri = "/wifi",      .method = HTTP_GET,  .handler = page_get };
    httpd_uri_t u_scan  = { .uri = "/wifi/scan", .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t u_save  = { .uri = "/wifi/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t u_root  = { .uri = "/",          .method = HTTP_GET,  .handler = root_redirect };
    httpd_register_uri_handler(s_http, &u_page);
    httpd_register_uri_handler(s_http, &u_scan);
    httpd_register_uri_handler(s_http, &u_save);
    httpd_register_uri_handler(s_http, &u_root);
    ESP_LOGI(TAG, "Wi-Fi config panel at http://rover.local/wifi (192.168.99.1)");
}

httpd_handle_t wifi_portal_httpd(void)
{
    return s_http;
}
