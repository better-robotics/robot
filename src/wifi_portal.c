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
/* Embed mode (?embed=1): the panel is iframed inside a dashboard card, so drop
 * its own topbar + trim outer padding and let the host card provide the chrome. */
".embed .topbar{display:none}.embed main{padding:14px 14px 20px}"
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
".btn{display:block;text-align:center;background:var(--accent);color:var(--accent-ink);"
"border-radius:var(--radius);padding:.75rem .85rem;margin:.6rem 0 0;font-weight:600;"
"text-decoration:none;min-height:var(--tap);box-sizing:border-box}"
"</style></head><body>"
/* Set the embed class synchronously before first paint (no topbar flash) when
 * the dashboard iframes this panel with ?embed=1. */
"<script>if(location.search.indexOf('embed')>=0)document.documentElement.className='embed'</script>"
"<header class=topbar><h1><span class=a>Better</span><span class=b>Robotics</span></h1></header>"
"<main>"
/* Live status first (#21 UX fix): the panel used to be write-only — no way to see
 * what's configured, whether a save landed, or where the dashboard is. */
"<div class=card>"
"<h2 id=bid>This board</h2>"
"<p class=s id=bst>checking status&#8230;</p>"
"<div id=bgo></div>"
"</div>"
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
/* Status card: poll /wifi/status. In embed mode the panel already sits inside the
 * dashboard, so the "open the dashboard" button is suppressed (it would navigate
 * the modal iframe into a nested dashboard). */
"async function stat(){try{let r=await fetch('/wifi/status');let j=await r.json();"
"document.getElementById('bid').textContent=j.board||'This board';"
"let t=j.role=='hub'?'Classroom hub':j.role=='rover'?'Rover (always joins a hub)':'Rover';"
"t+=' \\u00b7 ';"
"t+=j.state=='hub'?'part of classroom '+j.uplink:"
"j.state=='local'?(j.ip?'self-hosted \\u00b7 internet via '+j.uplink+' ('+j.ip+')':"
"'self-hosted \\u00b7 no internet uplink (drives fine offline)'):"
"j.state=='remote'?'on '+j.uplink+(j.ip?' ('+j.ip+')':''):"
"'looking for a network\\u2026';"
"if(j.pin)t+=' \\u00b7 pinned to '+j.pin;"
"document.getElementById('bst').textContent=t;"
"let g=document.getElementById('bgo');g.textContent='';"
"if(j.dash&&document.documentElement.className!='embed'){"
"let a=document.createElement('a');a.className='btn';a.href=j.dash;"
"a.textContent=j.dash=='/'?'Open the dashboard':'Open the class dashboard';g.appendChild(a)}"
"}catch(e){}setTimeout(stat,5000)}"
"stat();"
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

/* The landing at "/", replacing a naked 302→/wifi (scar 2026-07-10: a first-time
 * user's very first screen was a Wi-Fi form — implying internet is required for a
 * product whose point is working without it — and nothing ever led them onward).
 * It polls /wifi/status and routes to wherever the drive dashboard actually is:
 * reload once THIS board serves it (start_ws_mqtt_bridge takes over "/", so the
 * reload lands on the dashboard), a button to the hub's copy when the board
 * joined a classroom (reachable from this AP via NAPT), a holding line while the
 * board is still deciding. /wifi stays one tap away. */
static const char LANDING[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>BetterRobotics</title><style>"
":root{color-scheme:dark;--bg:#0d0f12;--surface:#16191d;--ink:#f2f3f5;--ink-muted:#aeb4bd;"
"--border:rgba(255,255,255,.10);--accent:#00539B;--accent-ink:#fff;--radius-lg:18px;--radius:12px;--tap:44px}"
"*{box-sizing:border-box}"
"body{margin:0;background:var(--bg);color:var(--ink);"
"font-family:-apple-system,BlinkMacSystemFont,'SF Pro Text',system-ui,'Helvetica Neue',Arial,sans-serif;"
"font-size:16px;line-height:1.47;-webkit-font-smoothing:antialiased;"
"padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}"
".topbar{display:flex;align-items:center;height:50px;padding:0 20px;border-bottom:.5px solid var(--border)}"
".topbar h1{font-size:17px;font-weight:600;letter-spacing:-.01em;margin:0}"
".topbar .a{font-weight:700}.topbar .b{font-weight:400;color:var(--ink-muted)}"
"main{max-width:32rem;margin:0 auto;padding:24px 16px 48px}"
".card{background:var(--surface);border:.5px solid var(--border);border-radius:var(--radius-lg);"
"padding:20px;margin-bottom:16px;box-shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px -12px rgba(0,0,0,.6)}"
".card h2{font-size:20px;font-weight:700;letter-spacing:-.015em;margin:0 0 6px}"
".s{color:var(--ink-muted);font-size:13px}"
".btn{display:block;text-align:center;background:var(--accent);color:var(--accent-ink);"
"border-radius:var(--radius);padding:.75rem .85rem;margin:.9rem 0 0;font-weight:600;"
"text-decoration:none;min-height:var(--tap)}"
".foot{text-align:center}.foot a{color:var(--ink-muted)}"
"</style></head><body>"
"<header class=topbar><h1><span class=a>Better</span><span class=b>Robotics</span></h1></header>"
"<main>"
"<div class=card><h2 id=bn>&#8230;</h2><p class=s id=st>Starting up&#8230;</p><div id=act></div></div>"
"<p class=\"s foot\"><a href=/wifi>Wi-Fi &amp; role settings</a></p>"
"</main>"
"<script>"
"let n=0;"
"function go(){fetch('/wifi/status').then(r=>r.json()).then(j=>{"
"document.getElementById('bn').textContent=j.board||'rover';"
"let st=document.getElementById('st'),act=document.getElementById('act');"
/* This board serves the dashboard now — "/" was re-registered, so reload IS it. */
"if(j.dash=='/'){location.reload();return}"
"if(j.dash){"
"st.textContent='Part of the classroom network'+(j.uplink?' '+j.uplink:'')+' \\u2014 drive it from the class dashboard.';"
/* DOM, not innerHTML: dash derives from a stored locator (user-supplied NVS). */
"if(!act.firstChild){let a=document.createElement('a');a.className='btn';a.href=j.dash;"
"a.textContent='Open the class dashboard';act.appendChild(a)}"
"setTimeout(go,5000);return}"
"act.innerHTML='';"
"st.textContent=n>12?'No hub found yet \\u2014 the rover keeps looking. You can set its Wi-Fi or role below.':"
"'Starting up \\u2014 deciding how to connect\\u2026';"
"n++;setTimeout(go,1200)}).catch(()=>{n++;setTimeout(go,2000)})}"
"go();"
"</script></body></html>";

static esp_err_t landing_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, LANDING, HTTPD_RESP_USE_STRLEN);
}

/* Live board state for the landing page + the panel's status card (facts owned
 * and serialized by hub_role.c — board_status_json). */
static esp_err_t status_get(httpd_req_t *req)
{
    char j[256];
    board_status_json(j, sizeof j);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
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

/* Drain a small form POST into buf (cap includes the NUL terminator). */
static void read_form_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = 0;
    while (total < (int)cap - 1) {
        int r = httpd_req_recv(req, buf + total, cap - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        total += r;
    }
    buf[total] = 0;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[256];
    read_form_body(req, body, sizeof body);

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
    read_form_body(req, body, sizeof body);

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
    cfg.max_uri_handlers = 12;   /* /wifi{,/scan,/save,/role×2,/status} + / here;
                                  * +/fleet & dashboard / later (bridge) = 9 peak */
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "config panel httpd (:80) failed to start");
        s_http = NULL;
        return;
    }
    httpd_uri_t u_page  = { .uri = "/wifi",        .method = HTTP_GET,  .handler = page_get };
    httpd_uri_t u_scan  = { .uri = "/wifi/scan",   .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t u_save  = { .uri = "/wifi/save",   .method = HTTP_POST, .handler = save_post };
    httpd_uri_t u_rget  = { .uri = "/wifi/role",   .method = HTTP_GET,  .handler = role_get };
    httpd_uri_t u_rpost = { .uri = "/wifi/role",   .method = HTTP_POST, .handler = role_post };
    httpd_uri_t u_stat  = { .uri = "/wifi/status", .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t u_root  = { .uri = "/",            .method = HTTP_GET,  .handler = landing_get };
    httpd_register_uri_handler(s_http, &u_page);
    httpd_register_uri_handler(s_http, &u_scan);
    httpd_register_uri_handler(s_http, &u_save);
    httpd_register_uri_handler(s_http, &u_rget);
    httpd_register_uri_handler(s_http, &u_rpost);
    httpd_register_uri_handler(s_http, &u_stat);
    httpd_register_uri_handler(s_http, &u_root);
    ESP_LOGI(TAG, "config panel on :80 (/wifi + /wifi/status; state-routing landing at /)");
}

httpd_handle_t wifi_portal_httpd(void)
{
    return s_http;
}
