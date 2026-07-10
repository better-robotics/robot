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
"<title>BetterRobotics</title><style>"
/* Shared HIG token system — mirrors hub/dashboard.html so this panel and the drive
 * dashboard read as one product: grouped-background dark, elevated cards, a
 * translucent blurred sticky topbar, 44px tap targets. Phone-first by design
 * (students open it on a phone): a single 32rem column, with safe-area insets. */
":root{color-scheme:dark;--bg:#0d0f12;--surface:#16191d;--inset:#1f242b;"
"--ink:#f2f3f5;--ink-muted:#aeb4bd;--ink-faint:#838a93;--border:rgba(255,255,255,.10);"
"--accent:#00539B;--accent-ink:#fff;--brand:#0577B1;--focus:#4aa3d6;"
"--radius-lg:18px;--radius:12px;--tap:44px;--topbar:50px}"
"*{box-sizing:border-box}"
"body{margin:0;background:var(--bg);color:var(--ink);"
"font-family:-apple-system,BlinkMacSystemFont,'SF Pro Text',system-ui,'Helvetica Neue',Arial,sans-serif;"
"font-size:16px;line-height:1.47;-webkit-font-smoothing:antialiased;"
"padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}"
".topbar{display:flex;align-items:center;height:var(--topbar);padding:0 20px;position:sticky;top:0;z-index:20;"
"border-bottom:.5px solid var(--border);background:color-mix(in srgb,var(--bg) 82%,transparent);"
"-webkit-backdrop-filter:saturate(180%) blur(20px);backdrop-filter:saturate(180%) blur(20px)}"
".topbar h1{font-size:17px;font-weight:600;letter-spacing:-.01em;margin:0}"
".topbar .a{font-weight:700;color:var(--ink)}"
".topbar .b{font-weight:400;color:var(--ink-muted)}"
"main{max-width:32rem;margin:0 auto;padding:24px 16px 48px}"
".card{background:var(--surface);border:.5px solid var(--border);border-radius:var(--radius-lg);"
"padding:20px;margin-bottom:16px;box-shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px -12px rgba(0,0,0,.6)}"
".card>h2{font-size:20px;font-weight:700;letter-spacing:-.015em;margin:0 0 6px}"
".s{color:var(--ink-muted);font-size:13px}"
"input,button,select{font:inherit;width:100%;box-sizing:border-box;padding:.7rem .85rem;margin:.35rem 0;"
"border-radius:var(--radius);border:.5px solid var(--border);background:var(--inset);color:var(--ink);min-height:var(--tap)}"
"input::placeholder{color:var(--ink-faint)}"
"button{background:var(--accent);color:var(--accent-ink);border:0;font-weight:600;cursor:pointer;"
"transition:filter .15s,transform .08s}button:hover{filter:brightness(1.08)}button:active{transform:scale(.98)}"
"input:focus,select:focus{outline:2.5px solid var(--focus);outline-offset:2px;border-color:transparent}"
/* Network list: inset rows, name left / signal right — an iOS list; each row is a
 * full 44px tap target (no min-height:0 override). */
"#nets button{background:var(--inset);border:.5px solid var(--border);color:var(--ink);text-align:left;"
"display:flex;justify-content:space-between;align-items:center;gap:.5rem;font-weight:500}"
"#msg,#rmsg{margin-top:1rem}"
"</style></head><body>"
"<header class=topbar><h1><span class=a>Better</span><span class=b>Robotics</span></h1></header>"
"<main>"
"<div class=card>"
"<h2>Set the rover&rsquo;s Wi-Fi</h2>"
"<p class=s>Pick your home network, enter its password, and the rover restarts to join it.</p>"
"<button onclick=scan()>Scan for networks</button>"
"<div id=nets></div>"
"<form onsubmit=\"return save(event)\">"
"<input id=ssid name=ssid placeholder=\"Network name\" autocapitalize=off autocorrect=off required>"
"<input id=pass name=pass type=password placeholder=\"Password (blank if open)\">"
"<button type=submit>Save &amp; restart</button></form>"
"<div id=msg></div>"
"</div>"
"<div class=card>"
"<h2>Board role</h2>"
"<p class=s>What this board is. Changing it restarts the board.</p>"
"<select id=role>"
"<option value=auto>Normal rover &#8212; drives; hosts itself at home</option>"
"<option value=hub>Classroom hub &#8212; hosts the class, no driving</option>"
"<option value=rover>Rover only &#8212; always joins a hub</option>"
"</select>"
"<button onclick=setrole()>Apply role &amp; restart</button>"
"<div id=rmsg></div>"
"</div>"
"</main>"
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
/* The rover stays your access point (Pi model): its STA leg is only the internet
 * uplink, so the student keeps their device on the rover's OWN Wi-Fi throughout. */
"let nm=document.createElement('b');nm.textContent=document.getElementById('ssid').value;"
"m.textContent='Saved. The rover is restarting to use ';m.appendChild(nm);"
"m.append(' for internet \\u2014 it stays your Wi-Fi. Keep this device on the rover\\u2019s network "
"(it reappears with the same name in ~10s), then reopen rover.local.')}"
"catch(x){m.textContent='Saved. The rover is restarting \\u2014 stay on its Wi-Fi and reopen rover.local.'}return false}"
/* Reflect the board's current role in the select on load. */
"fetch('/wifi/role').then(r=>r.json()).then(j=>{document.getElementById('role').value=j.role}).catch(()=>{});"
"async function setrole(){let rm=document.getElementById('rmsg');rm.textContent='applying\\u2026';"
"let v=document.getElementById('role').value;let f=new URLSearchParams();f.append('role',v);"
"try{await fetch('/wifi/role',{method:'POST',body:f});"
"rm.textContent=v=='hub'?'Now the classroom hub. It restarts and appears as an open hub-\\u2026 network (hub.local); rovers auto-join it.':"
"v=='rover'?'Rover-only. Restarting \\u2014 it will keep looking for a hub to join.':"
"'Normal rover. Restarting \\u2014 stay on its Wi-Fi and reopen rover.local.'}"
"catch(x){rm.textContent='Applied \\u2014 restarting\\u2026'}}"
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
    ESP_LOGW(TAG, "config-apply restart (new Wi-Fi or role)");
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

/* ── Board role (#2 tier-2 designate) ─────────────────────────────────────────
 * role_pref selects the boot dispatch (main.c): auto/rover → board_run, hub →
 * hub_role_run. Exposing it here is what lets a professor turn any board into the
 * classroom hub — and back — without reflashing. The hub ALSO serves this panel
 * (hub_role_run calls wifi_portal_start), so designating HUB isn't a one-way trip. */
static const char *role_str(rover_role_pref_t r)
{
    return r == ROLE_HUB ? "hub" : r == ROLE_ROVER ? "rover" : "auto";
}

static esp_err_t role_get(httpd_req_t *req)
{
    char j[32];
    snprintf(j, sizeof j, "{\"role\":\"%s\"}", role_str(rover_config_load_role_pref()));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

static esp_err_t role_post(httpd_req_t *req)
{
    char body[64];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int r = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        total += r;
    }
    body[total] = 0;

    char role[8];
    if (!form_field(body, "role", role, sizeof role)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing role");
    }
    rover_role_pref_t rp = strcmp(role, "hub")   == 0 ? ROLE_HUB
                         : strcmp(role, "rover") == 0 ? ROLE_ROVER
                         : ROLE_AUTO;   /* unknown → the safe default */
    esp_err_t e = rover_config_set_role_pref(rp);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rover_config_set_role_pref failed: %s", esp_err_to_name(e));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "role set to '%s' — restarting into the new dispatch", role_str(rp));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(reboot_task, "role-reboot", 2048, NULL, 5, NULL);
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
    httpd_uri_t u_rget  = { .uri = "/wifi/role", .method = HTTP_GET,  .handler = role_get };
    httpd_uri_t u_rpost = { .uri = "/wifi/role", .method = HTTP_POST, .handler = role_post };
    httpd_uri_t u_root  = { .uri = "/",          .method = HTTP_GET,  .handler = root_redirect };
    httpd_register_uri_handler(s_http, &u_page);
    httpd_register_uri_handler(s_http, &u_scan);
    httpd_register_uri_handler(s_http, &u_save);
    httpd_register_uri_handler(s_http, &u_rget);
    httpd_register_uri_handler(s_http, &u_rpost);
    httpd_register_uri_handler(s_http, &u_root);
    ESP_LOGI(TAG, "config panel on :80 (/wifi — Wi-Fi + board role)");
}

httpd_handle_t wifi_portal_httpd(void)
{
    return s_http;
}
