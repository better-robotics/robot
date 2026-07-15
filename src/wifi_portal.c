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
#include "esp_timer.h"       /* esp_timer_get_time — the captive-ack idle window */
#include "esp_netif.h"       /* the AP's own IP, for the RFC 8908 captive-portal-api body */
#include "cJSON.h"          /* /wifi/connect speaks hubd's JSON dialect */
#include "roles.h"          /* board_ap_t, board_wifi_scan */
#include "rover_config.h"   /* rover_config_set_wifi, rover_config_load */
#include "wifi_portal.h"
#include "dns_server.h"     /* wildcard :53 responder — makes the OS captive-portal
                             * probes below actually get dialed against this board */
#include "lwip/sockets.h"   /* lwip_getpeername — captive_accepted()'s per-client IP */

static const char *TAG = "wifi-portal";
static httpd_handle_t s_http = NULL;

#define SCAN_MAX 24

/* ── The one UI vocabulary these pages share ──────────────────────────────────
 * All three portal pages (/wifi, /, /welcome) are standalone documents — the
 * board serves them with no internet, so they can't <link> a stylesheet. That
 * constraint used to mean three hand-maintained CSS copies, and they drifted
 * exactly as you'd expect: /wifi still had a wall-of-pills network list and a
 * full-width primary-blue button for EVERY button long after the dashboard
 * fixed both (2026-07-16 audit).
 *
 * So: one HEAD constant, streamed as its own chunk (httpd_resp_send_chunk —
 * /welcome already sent itself in pieces to splice AP_BASE). Three pages, one
 * copy in flash instead of three, one place to fix.
 *
 * Mirrors hub/dashboard.html's encoded system rather than describing it:
 * the base <button> IS the neutral tile (so a classless button is in-system,
 * not primary-blue), tiers are classes, sizes come from tokens only, and
 * .list-group is the iOS grouped-inset list — ONE filled panel with hairline
 * separators, which is what iPhone/Android Settings › Wi-Fi actually are.
 * Keep in sync with dashboard.html on touch; the tokens must match byte-for-
 * byte (hub@f005439 converged them with better-robotics.github.io+workbench). */
static const char HEAD[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>BetterRobotics</title><style>"
":root{color-scheme:dark;--bg:#1a1d20;--surface:#212529;--inset:#2a3037;"
"--ink:#e9ecef;--ink-muted:#adb5bd;--ink-faint:#8b929b;"
"--border:rgba(255,255,255,0.10);--border-strong:rgba(255,255,255,0.18);"
"--accent:#00539B;--accent-ink:#ffffff;--accent-soft:rgba(5,119,177,0.16);"
"--link:#4c9fd4;--brand:#0577B1;--focus:#4aa3d6;--danger:#ff453a;--warn-ink:#e8a900;"
"--radius-lg:14px;--radius:8px;--radius-pill:999px;--tap:44px;--ctrl-h:2.2rem;--topbar:50px;"
"--fs-small:0.78rem;--fs-body:0.85rem}"
"*{box-sizing:border-box}"
"[hidden]{display:none!important}"
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
".card h2{font-size:20px;font-weight:700;letter-spacing:-.015em;margin:0 0 6px}"
".s{color:var(--ink-muted);font-size:13px}"
/* Control vocabulary — the base is the neutral tile, tiers layer on. A bare
 * <button> used to be primary blue here (button{background:var(--accent)}),
 * which is why "Scan for networks" shouted louder than anything it sat beside. */
"input,select,textarea{font:inherit}"
/* inline-flex + gap centres icon and label on both axes with no per-glyph
 * magic number, and fixes text riding high inside min-height — block layout
 * doesn't centre vertically, so an <a class=btn> sat ~5.6px above centre once
 * pointer:coarse raised min-height to 44px (i.e. on every phone). */
"button,.btn{display:inline-flex;align-items:center;justify-content:center;gap:.4rem;"
"cursor:pointer;font:inherit;font-size:var(--fs-body);font-weight:600;"
"min-height:var(--ctrl-h);padding:.4rem .8rem;border-radius:var(--radius);"
"border:.5px solid var(--border);color:var(--ink);background:var(--inset);"
"transition:filter .15s,transform .08s}"
"button:hover,.btn:hover{filter:brightness(1.25)}"
"button:active,.btn:active{transform:scale(.97)}"
"button:disabled{opacity:.55;cursor:default;filter:none;transform:none}"
".btn-primary{color:var(--accent-ink);background:var(--accent);border-color:transparent}"
".btn-danger{color:var(--danger)}"
/* A link that wears a button: block-level, full width. */
".btn{display:flex;width:100%;text-align:center;text-decoration:none;margin:.9rem 0 0}"
".link-btn{background:none;border:none;padding:0;min-height:0;font-weight:400;color:var(--link);"
"width:auto;display:inline;margin:0;gap:0}"
"input,select{background:var(--inset);color:var(--ink);font-size:var(--fs-body);width:100%;"
"border:.5px solid var(--border);border-radius:var(--radius);padding:.4rem .7rem;min-height:var(--ctrl-h)}"
"input::placeholder{color:var(--ink-faint)}"
":focus-visible{outline:2px solid var(--focus);outline-offset:1px}"
/* --tap under a finger only — a mouse never needed 44px. */
"@media(pointer:coarse){button,.btn,input,select,.net{min-height:var(--tap)}.link-btn{min-height:0}}"
/* iOS grouped-inset list: ONE panel, hairline separators. Rows bring layout
 * only — a row with its own fill/border is the wall-of-pills bug. */
".list-group{display:flex;flex-direction:column;overflow:hidden;background:var(--inset);"
"border:.5px solid var(--border);border-radius:var(--radius);margin:.6rem 0 0}"
".list-group>*{border:none;border-radius:0;background:transparent;width:100%;margin:0}"
".list-group>*+*{border-top:.5px solid var(--border)}"
".list-group>*:hover{background:color-mix(in srgb,var(--ink) 7%,transparent);filter:none}"
".list-group>*:active{transform:none}"
".net{display:flex;justify-content:space-between;align-items:center;gap:.6rem;"
"text-align:left;font-weight:500;padding:.55rem .9rem;min-height:var(--ctrl-h)}"
".list-note{padding:.7rem .9rem;color:var(--ink-muted);font-size:var(--fs-body)}"
".stack{display:flex;flex-direction:column;gap:.6rem;min-width:0}";

/* Every page = HEAD (chrome + the vocabulary) + its own CSS + body. Chunked,
 * so the shared core lives once. */
static esp_err_t send_head(httpd_req_t *req, const char *page_css)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (httpd_resp_send_chunk(req, HEAD, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    if (page_css && httpd_resp_send_chunk(req, page_css, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, "</style></head><body>", HTTPD_RESP_USE_STRLEN);
}

/* The config page. Self-contained (no external CSS/JS/fonts — it is served by the
 * board with no internet): Scan lists visible networks by name/signal; tapping one
 * fills the SSID; Save posts and the board restarts onto the chosen network. */
static const char PAGE_CSS[] =
/* Embed mode (?embed=1): the panel is iframed inside a dashboard card, so drop
 * its own topbar + trim outer padding and let the host card provide the chrome. */
".embed .topbar{display:none}.embed main{padding:14px 14px 20px}"
"#msg,#rmsg{margin-top:1rem}"
"form{margin:.6rem 0 0}"
"form input{margin:.35rem 0}"
/* The join row rides inside the list, under the network you tapped. */
/* Grid, not a wrapping flex row — the SSID owns its line, input+Connect share
 * the next, so a long name can't strand Connect on a ragged third line. */
"#join{display:grid;grid-template-columns:1fr auto;gap:.5rem;align-items:center;"
"padding:.6rem .9rem;background:var(--accent-soft)!important}"
"#join input{min-width:0;margin:0}"
"#jssid{grid-column:1/-1;font-size:var(--fs-body);min-width:0;overflow-wrap:anywhere}"
".foot{display:flex;gap:.9rem;flex-wrap:wrap;margin-top:.6rem;font-size:var(--fs-body)}"
/* Re-inks .s, never a second note style: the ONE warn hue = "act here", and it
 * recedes to plain ink once a password is set (same rule the dashboard's
 * identity chip follows). Carried with a glyph + words, never hue alone. */
".warn{color:var(--warn-ink);font-weight:600}";

static const char PAGE_BODY[] =
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
"<p class=s>Pick your home network and the rover restarts to join it.</p>"
/* No Scan button: opening the page IS the scan request, same as the dashboard
 * picker and as iPhone/Android. Rescan is a quiet link in .foot. */
"<div id=nets class=list-group></div>"
"<div id=join hidden><strong id=jssid></strong>"
"<input id=pass type=password placeholder=\"Network password\" autocapitalize=off autocorrect=off>"
"<button id=go class=btn-primary>Connect</button></div>"
"<div id=msg class=s></div>"
"<p class=foot><button id=rescan class=link-btn>Rescan</button></p>"
"</div>"
"<div class=card>"
"<h2>Board role</h2>"
"<p class=s>What this board is. Changing it restarts the board.</p>"
"<select id=role>"
"<option value=auto>Normal rover &#8212; drives; hosts itself at home</option>"
"<option value=hub>Classroom hub &#8212; hosts the class, no driving</option>"
"<option value=rover>Rover only &#8212; always joins a hub</option>"
"</select>"
"<button onclick=setrole() class=btn>Apply role &amp; restart</button>"
"<div id=rmsg class=s></div>"
/* Professor password: only a HUB board ever checks it (connect_cb), so it
 * lives in the role card. NVS, not a build flag — the compile-time literal is
 * plaintext in the image and firmware.yml publishes .bins from a public repo,
 * so baking a real classroom's password in would ship it to every downloader.
 * Blank = keep whatever is stored (so the field can render without leaking it);
 * "-" clears back to the compile-time default. */
"<div id=profwrap hidden>"
"<h2 style=\"margin-top:20px\">Professor password</h2>"
/* Shown only while NVS is unset. Without it the placeholder is silent: the one
 * gated action in the whole classroom sits behind a string anyone can read out
 * of a published .bin, and nothing anywhere says so. */
"<p class=\"s warn\" id=profwarn hidden>&#9888; Still the built-in password &#8212; it ships in every firmware download, so anyone on this Wi-Fi can stop the whole class. Set a real one below.</p>"
"<p class=s id=profnote>Gates the fleet-wide emergency stop. Leave blank to keep the current one; &quot;-&quot; resets it to the built-in default.</p>"
"<input id=profpass type=password placeholder=\"new professor password\" autocapitalize=off autocorrect=off>"
"<button onclick=setprof() class=btn>Save password</button>"
"<div id=pmsg class=s></div>"
"</div>"
"</div>"
"</main>"
"<script>"
"let chosen=null,scanning=false;"
"const nets=document.getElementById('nets'),join=document.getElementById('join'),"
"pass=document.getElementById('pass'),msg=document.getElementById('msg');"
"const note=t=>{nets.innerHTML='';let p=document.createElement('p');p.className='list-note';"
"p.textContent=t;nets.appendChild(p)};"
/* join is MOVED into the list; park it before any rebuild or it's deleted. */
"function park(){join.hidden=true;document.getElementById('msg').before(join)}"
"async function scan(){if(scanning)return;scanning=true;park();note('Scanning\\u2026');"
"let a;try{a=await(await fetch('/wifi/scan')).json()}"
"catch(e){note('Scan failed \\u2014 tap Rescan to try again.');scanning=false;return}"
"finally{scanning=false}"
"if(!a.length){note('No networks found.');return}"
"a.sort((x,y)=>y.signal-x.signal);"
"nets.innerHTML='';"
"a.forEach(n=>{let b=document.createElement('button');b.type='button';b.className='net';"
/* textContent, NOT innerHTML: an SSID is attacker-controllable (any nearby AP can
 * name itself with markup), so it must never be parsed as HTML. */
"let s1=document.createElement('span');s1.textContent=n.ssid;"
"let s2=document.createElement('span');s2.className='s';"
"s2.textContent=(n.security==='NO'?'open':n.security)+' \\u00b7 '+n.signal+'%';"
"b.append(s1,s2);"
"b.onclick=()=>{chosen=n.ssid;b.after(join);join.hidden=false;"
"document.getElementById('jssid').textContent=n.ssid;"
"pass.style.display=n.security==='NO'?'none':'block';pass.value='';"
"(n.security==='NO'?document.getElementById('go'):pass).focus();"
"join.scrollIntoView({block:'nearest'})};"
"nets.appendChild(b)})}"
"document.getElementById('rescan').addEventListener('click',scan);"
"scan();"
/* /wifi/connect trial-joins and only persists a verified join (connect_post),
 * so a typo comes back as an error instead of a saved-and-rebooted dead board. */
"document.getElementById('go').addEventListener('click',async()=>{"
"if(!chosen)return;msg.textContent='Trying to join '+chosen+'\\u2026 (up to ~20s)';"
"let res;try{res=await(await fetch('/wifi/connect',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:chosen,password:pass.value})})).json()}"
"catch(e){msg.textContent='Connect request failed \\u2014 try again.';return}"
"if(res.ok){park();"
"msg.textContent='Joined \\u2713 \\u2014 saved; restarting to settle in. The rover stays your "
"Wi-Fi ('+chosen+' is only its internet uplink), but it blips off for ~10 seconds. If your phone "
"doesn\\u2019t re-join the rover\\u2019s Wi-Fi by itself, pick it again in Wi-Fi settings, then "
"reopen rover.local.';setTimeout(()=>location.reload(),12000)}"
"else{msg.textContent='Couldn\\u2019t join: '+(res.error||'check the password and try again.')}"
"});"
/* ONE /wifi/role fetch feeds every role-dependent control — the select's value,
 * whether the password field exists at all (hub-only; on a rover it's a control
 * that does nothing), and whether that field is still warning. It was two
 * identical fetches, which is how one of them ended up commented "/wifi/status
 * carries role" while calling /wifi/role. */
"function setprofwarn(d){document.getElementById('profwarn').hidden=!d}"
"fetch('/wifi/role').then(r=>r.json()).then(j=>{"
"document.getElementById('role').value=j.role;"
"document.getElementById('profwrap').hidden=j.role!='hub';"
"setprofwarn(j.prof_default)}).catch(()=>{});"
/* Status card: poll /wifi/status. In embed mode the panel already sits inside the
 * dashboard, so the "open the dashboard" button is suppressed (it would navigate
 * the modal iframe into a nested dashboard). */
"async function stat(){try{let r=await fetch('/wifi/status');let j=await r.json();"
"document.getElementById('bid').textContent=j.board||'This board';"
"let t=j.role=='hub'?'Classroom hub':j.role=='rover'?'Rover (always joins a hub)':'Rover';"
"t+=' \\u00b7 ';"
"t+=j.state=='hub'?'part of classroom '+j.ssid:"
"j.state=='local'?(j.ip?'self-hosted \\u00b7 internet via '+j.ssid+' ('+j.ip+')':"
"'self-hosted \\u00b7 no internet uplink (drives fine offline)'):"
"j.state=='remote'?'on '+j.ssid+(j.ip?' ('+j.ip+')':''):"
"'looking for a network\\u2026';"
"if(j.pin)t+=' \\u00b7 pinned to '+j.pin;"
"document.getElementById('bst').textContent=t;"
"let g=document.getElementById('bgo');g.textContent='';"
"if(j.dash&&document.documentElement.className!='embed'){"
"let a=document.createElement('a');a.className='btn btn-primary';a.href=j.dash;a.target='_blank';a.rel='noopener';"
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
"async function setprof(){let m=document.getElementById('pmsg');"
"let v=document.getElementById('profpass').value;"
"if(!v){m.textContent='Enter a password, or \\u2212 to reset to the built-in default.';return}"
"m.textContent='saving\\u2026';"
"let f=new URLSearchParams();f.append('pass',v);"
"try{let r=await(await fetch('/wifi/professor',{method:'POST',body:f})).json();"
"m.textContent=r.ok?(v=='-'?'Reset to the built-in default. New connections use it immediately.':"
"'Saved. New professor connections use it immediately \\u2014 no restart needed.'):"
"('Couldn\\u2019t save: '+(r.error||'try again.'));"
/* The warning tracks what was actually stored: clearing puts the public
 * default back, so the amber returns rather than staying dismissed. */
"if(r.ok)setprofwarn(v=='-');"
"document.getElementById('profpass').value=''}"
"catch(x){m.textContent='Save failed \\u2014 try again.'}}"
"</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    if (send_head(req, PAGE_CSS) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, PAGE_BODY, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* The landing at "/", replacing a naked 302→/wifi (scar 2026-07-10: a first-time
 * user's very first screen was a Wi-Fi form — implying internet is required for a
 * product whose point is working without it — and nothing ever led them onward).
 * It polls /wifi/status and routes to wherever the drive dashboard actually is:
 * reload once THIS board serves it (start_ws_mqtt_bridge takes over "/", so the
 * reload lands on the dashboard), a button to the hub's copy when the board
 * joined a classroom (reachable from this AP via NAPT), a holding line while the
 * board is still deciding. /wifi stays one tap away. */
static const char LANDING_CSS[] =
".foot{text-align:center}.foot a{color:var(--ink-muted)}";

static const char LANDING_BODY[] =
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
"st.textContent='Part of the classroom network'+(j.ssid?' '+j.ssid:'')+' \\u2014 drive it from the class dashboard.';"
/* DOM, not innerHTML: dash derives from a stored locator (user-supplied NVS). */
"if(!act.firstChild){let a=document.createElement('a');a.className='btn btn-primary';a.href=j.dash;a.target='_blank';a.rel='noopener';"
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
    if (send_head(req, LANDING_CSS) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, LANDING_BODY, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Live board state for the landing page + the panel's status card (facts owned
 * and serialized by hub_role.c — board_status_json). */
static esp_err_t status_get(httpd_req_t *req)
{
    char j[448];   /* the venue portal_url rides along since 2026-07-14 */
    board_status_json(j, sizeof j);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    board_ap_t *aps = malloc(SCAN_MAX * sizeof *aps);
    if (!aps) { httpd_resp_send_500(req); return ESP_FAIL; }
    int n = board_wifi_scan(aps, SCAN_MAX);

    /* Each row ~ {"ssid":"<=32","signal":100,"security":"WPA2"}, — budget
     * generously. The shape is the Pi hubd's /wifi/scan dialect (wifi.rs
     * scan/map_auth) — the shared dashboard reads `signal` (0-100) and gates
     * the password box on `security === "NO"`; the old {rssi, open} keys
     * rendered as "undefined%" and asked open networks for a password. */
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
        /* dBm → percent, the common 2×(rssi+100) clamp: -50 dBm ≥ 100%,
         * -100 dBm = 0%. The scan can't distinguish WPA flavors (only
         * open/locked), so locked networks badge as the common case. */
        int pct = 2 * (aps[i].rssi + 100);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        off += snprintf(json + off, cap - off, "%s{\"ssid\":\"%s\",\"signal\":%d,\"security\":\"%s\"}",
                        i ? "," : "", esc, pct, aps[i].open ? "NO" : "WPA2");
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

/* POST /wifi/forget — "Forget this network" (dashboard's Set-up-Wi-Fi panel).
 * No body needed: erases the stored uplink only (rover_config_clear_wifi) and
 * reboots, same config-apply path as save_post. The board comes back up with
 * no venue/home network configured — a fresh island, same as never-provisioned. */
static esp_err_t forget_post(httpd_req_t *req)
{
    esp_err_t e = rover_config_clear_wifi();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rover_config_clear_wifi failed: %s", esp_err_to_name(e));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "forgot the stored uplink — restarting as a fresh island");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "forgotten");
    xTaskCreate(reboot_task, "cfg-reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* POST /wifi/connect — hubd's JSON dialect: body {ssid, password}, reply
 * {"ok":true} / {"ok":false,"error":…}. The shared dashboard's Set-up-Wi-Fi
 * panel drives THIS path on both hubs (dashboard.html wifi-connect); the
 * form-POST /wifi/save above stays as the portal page's own submit.
 * ok now means VERIFIED, matching the Pi hubd (whose nmcli connect blocks
 * until the join lands): the dedicated hub re-dials live and lets its panel
 * watch the join; board/rover mode trial-joins first — the AP and this page
 * stay up through the attempt — and only a verified join is persisted to
 * NVS and config-apply rebooted. A failed trial replies with the verdict
 * ("wrong password?") and saves nothing, so a typo can no longer be saved
 * and rebooted into. */
static esp_err_t connect_post(httpd_req_t *req)
{
    char body[256];
    read_form_body(req, body, sizeof body);   /* raw body reader, despite the name */

    cJSON *j = cJSON_Parse(body);
    const cJSON *jssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    const cJSON *jpass = j ? cJSON_GetObjectItem(j, "password") : NULL;
    char ssid[33] = "", pass[65] = "";
    if (cJSON_IsString(jssid)) snprintf(ssid, sizeof ssid, "%s", jssid->valuestring);
    if (cJSON_IsString(jpass)) snprintf(pass, sizeof pass, "%s", jpass->valuestring);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing ssid\"}");
    }
    /* The dedicated hub re-dials live — the AP (and the phone on it, and this
     * very panel) stays up; the panel's status re-read shows the join land.
     * Its loop doesn't own the radio, so the swap is safe fire-and-forget. */
    if (board_wifi_redial(ssid, pass)) {
        if (rover_config_set_wifi(ssid, pass) != ESP_OK)
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not save credentials\"}");
        ESP_LOGI(TAG, "saved uplink '%s' via /wifi/connect — re-dialing live", ssid);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    /* Board/rover mode: the apply is still a config-apply reboot, but the
     * credentials are trial-joined FIRST (blocking, up to ~20 s; the AP and
     * this page ride through it). Only a verified join is persisted — a
     * typo'd password comes back as this reply's error instead of being
     * saved, rebooted into, and discovered as a dead board. */
    const char *why = board_wifi_try_join(ssid, pass);
    if (why) {
        char reply[128];
        snprintf(reply, sizeof reply, "{\"ok\":false,\"error\":\"%s\"}", why);
        return httpd_resp_sendstr(req, reply);
    }
    if (rover_config_set_wifi(ssid, pass) != ESP_OK)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not save credentials\"}");
    ESP_LOGI(TAG, "uplink '%s' verified via /wifi/connect — saving + restarting to settle in", ssid);
    httpd_resp_sendstr(req, "{\"ok\":true}");
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
    /* prof_default mirrors connect_cb's own test (`nvs_pass[0] ? nvs : baked`):
     * unset NVS means this hub still admits the compile-time PROFESSOR_PASS,
     * which ships in every published .bin — i.e. Stop-all is gated by a public
     * string. Discloses nothing: anyone on the open AP can establish the same
     * fact with one CONNECT. Told here, not polled from /wifi/status — it
     * changes on a save, never on its own, and this is the fetch that already
     * decides whether the password field exists at all. */
    char nvs_pass[65];
    rover_config_load_professor_pass(nvs_pass);
    char j[64];
    snprintf(j, sizeof j, "{\"role\":\"%s\",\"prof_default\":%s}",
             role_str(rover_config_load_role_pref()),
             nvs_pass[0] ? "false" : "true");
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

/* POST /wifi/professor — body `pass=<value>`. Sets the hub role's professor
 * password in NVS (rover_config_set_professor_pass), which connect_cb reads
 * per-connection, so a rotation takes effect on the next connect with no
 * reboot. "-" clears back to the compile-time PROFESSOR_PASS.
 *
 * NVS and not a build flag: PROFESSOR_PASS is a plaintext literal in the
 * image, `robot` is a public repo, and firmware.yml uploads flashable .bins —
 * a baked-in secret ships to everyone who downloads one, and `strings
 * firmware.bin` reads it out. This keeps a real classroom's password off the
 * shared image and per-board.
 *
 * Never echoes the stored value back: the page can set it, not read it. */
static esp_err_t professor_post(httpd_req_t *req)
{
    char body[128];
    read_form_body(req, body, sizeof body);
    char pass[80];
    httpd_resp_set_type(req, "application/json");
    if (!form_field(body, "pass", pass, sizeof pass) || !pass[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing password\"}");
    }
    /* "-" is the clear sentinel, mirroring cmd/config's hub-pin convention.
       An empty stored secret would admit EVERY client as professor, so
       rover_config_set_professor_pass("") erases instead of storing. */
    const bool clearing = strcmp(pass, "-") == 0;
    esp_err_t e = rover_config_set_professor_pass(clearing ? "" : pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set professor pass failed: %s", esp_err_to_name(e));
        return httpd_resp_sendstr(req,
            e == ESP_ERR_INVALID_ARG ? "{\"ok\":false,\"error\":\"too long (max 64)\"}"
                                     : "{\"ok\":false,\"error\":\"could not save\"}");
    }
    /* Never log the value. */
    ESP_LOGW(TAG, "professor password %s", clearing ? "reset to the built-in default" : "changed");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Captive-portal Accept flip (ports hub/pi/src/bin/hubd.rs's Accept design to
 * this firmware, 2026-07-14 — a live board's captive sheet was caught
 * rendering the FULL dashboard inside itself, the exact failure the Pi's
 * design exists to avoid). A device that tapped Continue on /welcome gets
 * genuine "you have internet" answers from probe_redirect from then on, which
 * is what lets the OS's Captive Network Assistant actually dismiss its sheet
 * — a redirect alone never triggers that, only a real success signature does.
 *
 * Per-client, keyed by the caller's own IPv4 (2026-07-16). The first cut here
 * tried exactly this and gave up: httpd_req_to_sockfd + plain getpeername()
 * into a sockaddr_in read back peer IP 0.0.0.0 every time, so it fell back to
 * one global flag — meaning a second phone joining within the idle window got
 * silently waved through with no Welcome prompt at all. Fine for a single
 * island rover, wrong for one being passed between students in a classroom.
 * Root cause (documented ESP-IDF quirk, espressif/esp-idf#4863): this httpd's
 * listener is IPv6 internally even for IPv4 connections, so a sockaddr_in
 * read comes back zeroed. The fix is lwip_getpeername() (not the newlib
 * getpeername() wrapper) into a sockaddr_in6 — the real IPv4 word lives at
 * sin6_addr.un.u32_addr[3] (client_ip(), below). ACCEPTED_MAX matches
 * AP_MAX_CONN (hub_role.c) — never more concurrent clients than the AP
 * itself allows; a full table evicts the oldest accept.
 *
 * Gated on board_has_uplink() (live-tested 2026-07-14): flipping
 * unconditionally worked exactly as designed on a pure-island board with no
 * uplink of its own — and broke the joining phone's real internet, because
 * "genuine success" IS iOS's signal to trust this Wi-Fi for real routing.
 * That's true and harmless when this board actually has a working uplink
 * (a classroom hub's venue Wi-Fi, or a rover joined to real home Wi-Fi with
 * NAT); it's a lie when there's no uplink at all, and iOS believed the lie.
 * So the flip only ever fires when board_has_uplink() is true right now —
 * checked live, not cached, since a board's uplink can come and go after
 * Continue was tapped. No uplink → captive_accepted() stays false forever,
 * the sheet never auto-dismisses, and /welcome's copy (below) says so
 * plainly instead of promising a Continue that can't deliver. */
#define ACKED_IDLE_US (15LL * 60 * 1000 * 1000)
#define ACCEPTED_MAX 8   /* == AP_MAX_CONN, hub_role.c */
static struct { uint32_t ip; int64_t accepted_us; } s_accepted[ACCEPTED_MAX];

/* The caller's IPv4, network byte order, or 0 on failure — see the comment
 * above: must be lwip_getpeername + sockaddr_in6, not plain getpeername. */
static uint32_t client_ip(httpd_req_t *req)
{
    struct sockaddr_in6 addr = {0};
    socklen_t len = sizeof(addr);
    int s = httpd_req_to_sockfd(req);
    if (s < 0 || lwip_getpeername(s, (struct sockaddr *)&addr, &len) != 0) return 0;
    return addr.sin6_addr.un.u32_addr[3];
}

static bool captive_accepted(uint32_t ip)
{
    if (!ip) return false;
    for (int i = 0; i < ACCEPTED_MAX; i++) {
        if (s_accepted[i].ip != ip) continue;
        if (esp_timer_get_time() - s_accepted[i].accepted_us > ACKED_IDLE_US) {
            s_accepted[i].ip = 0;
            return false;
        }
        return board_has_uplink();
    }
    return false;
}

static esp_err_t captive_ack_post(httpd_req_t *req)
{
    uint32_t ip = client_ip(req);
    if (ip) {
        int slot = -1, oldest_i = 0;
        for (int i = 0; i < ACCEPTED_MAX; i++) {
            if (s_accepted[i].ip == ip) { slot = i; break; }
            if (slot < 0 && s_accepted[i].ip == 0) slot = i;
            if (s_accepted[i].accepted_us < s_accepted[oldest_i].accepted_us) oldest_i = i;
        }
        if (slot < 0) slot = oldest_i;   /* table full — evict the oldest accept */
        s_accepted[slot].ip = ip;
        s_accepted[slot].accepted_us = esp_timer_get_time();
    }
    esp_ip4_addr_t logip = { .addr = ip };
    ESP_LOGI(TAG, "captive/ack from " IPSTR " — probes now answer genuine success", IP2STR(&logip));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

/* /welcome — the ONLY page an unacked captive-portal probe ever redirects to.
 * Deliberately dashboard-free itself: the dashboard never LOADS inside this
 * sheet (Apple's CNA sandboxes localStorage away from Safari, so a sign-in
 * made in here would silently vanish the moment the sheet closes) — but
 * reaching it is this page's one job, and that doesn't need to wait on
 * anything. The rover's own AP stays up regardless of uplink (always-APSTA),
 * so the dashboard is already reachable the instant a phone joins; "Open the
 * dashboard" is the first, unconditional thing offered here (2026-07-15 —
 * this used to be the LAST thing offered, behind a full scan/connect wizard
 * that duplicated what the dashboard's own Set-up-Wi-Fi panel already does
 * against these same /wifi/scan + /wifi/connect endpoints). Configuring
 * Wi-Fi, if the student wants internet, happens there now — opened in the
 * phone's real browser, never in this sandbox.
 *
 * Continue is a separate, narrower job: it's what calls /captive/ack, which
 * is what lets the OS's OWN captive sheet auto-dismiss (genuine-success
 * probe answers) instead of sitting on a dead Cancel button. That signal
 * doubles as "trust this Wi-Fi for real routing" to the OS, so it only
 * appears once board_has_uplink() is true server-side (captive_accepted())
 * — offering it sooner would tell a phone it has internet through a board
 * that doesn't, which breaks the phone's own cellular fallback (hardware-
 * verified 2026-07-14, this same file). Continue only ever helps the sheet
 * close cleanly; it was never the only way to the dashboard. */
/* Split so welcome_get() can splice a runtime AP_BASE between them (the AP's
 * own IP, computed once at wifi_portal_start() — see s_ap_base below). WELCOME
 * used to just do `d0.href=j.dash||'/'`, which is a bare relative path in the
 * common case (board_net_state_set(..., "/") — hub_role.c). A target=_blank
 * tap on that relative href only ever escapes the captive sandbox to a real
 * browser tab reliably when it resolves to an address the OS is willing to
 * hand off — exactly the "never redirect a walled-garden client to a
 * hostname" problem s_welcome_url already solves for the initial redirect
 * (2026-07-15, live-reported: the link visibly did nothing on iOS's CNA). */
static const char WELCOME_BODY[] =
"<header class=topbar><h1><span class=a>Better</span><span class=b>Robotics</span></h1></header>"
"<main>"
"<div class=card id=before><h2>Welcome</h2>"
"<p class=s id=lede>Checking this board&rsquo;s internet&#8230;</p>"
"<a class=\"btn btn-primary\" id=dashlink0 href=/ target=_blank rel=noopener>Open the dashboard</a>"
/* Venue-gate path (uplink=='portal'): the network the BOARD joined has its
 * own captive sign-in; behind the NAT every client shares the board's
 * venue-side MAC, so one sign-in from this phone clears it for the board.
 * The one thing on this page that genuinely can't move to the dashboard —
 * it has to happen from inside this specific captive session. */
"<a class=\"btn btn-primary\" id=venue-go hidden>Sign in</a>"
"<button class=\"btn btn-primary\" id=go hidden>Continue</button>"
"</div>"
"<div class=card id=after hidden><h2>You're set</h2>"
"<p class=s>Now open the dashboard in your regular browser, not this pop-up &#8212; "
"this pop-up forgets sign-ins when it closes.</p>"
"<a class=\"btn btn-primary\" id=dashlink href=/ target=_blank rel=noopener>Open the dashboard</a></div>"
"</main>"
"<script>";
/* welcome_get() sends "const AP_BASE='http://a.b.c.d';" here, then this. */
static const char WELCOME_POST[] =
"let lede=document.getElementById('lede'),go=document.getElementById('go'),"
"d0=document.getElementById('dashlink0');"
"let tries=0;"
/* AP_BASE is this board's own literal IP — always safe to hand a real
 * browser tab regardless of what hostname the captive sheet itself loaded
 * this page as. A dash already starting with 'http' (the remote/hub-joined
 * case, hub_role.c's board_run) is already absolute and passes through. */
"function abs(u){return u&&u.indexOf('http')==0?u:AP_BASE+(u||'/')}"
/* ?done=1 is the post-Continue view. Continue REACHES it by a real navigation
 * (location.href), never a DOM swap: the captive sheet re-runs its probe only
 * on a full-page load, so an AJAX-only accept leaves the sheet stuck on
 * Cancel with no auto-dismiss (WBA/Purple implementer consensus; the Cisco
 * ISE CNA bug is exactly this). */
"if(location.search.indexOf('done')>=0){"
"document.getElementById('before').hidden=true;"
"document.getElementById('after').hidden=false;"
"fetch('/wifi/status').then(r=>r.json()).then(j=>{"
"document.getElementById('dashlink').href=abs(j.dash)}).catch(()=>{})"
"}else{refresh()}"
/* Poll, don't check once: right after a connect-reboot the phone rejoins and
 * this sheet reopens while the STA is still mid-join — a single check showed
 * stale status ("didn't I just do this?"). Whether Continue can do anything
 * stays decided server-side (captive_accepted() gates on board_has_uplink())
 * — promising it before a real uplink would promise an auto-dismiss that can
 * never happen. dashlink0 stays live the whole time regardless. */
"function refresh(){"
"fetch('/wifi/status').then(r=>r.json()).then(j=>{"
/* The primary action must name what it actually opens. dash is "" whenever
 * this board hosts no dashboard (SEARCHING, and ROVER-pinned with no hub in
 * range — hub_role.c never self-brokers there), and abs("") resolves to the
 * board's own "/", which in that state is the LANDING router, not a
 * dashboard. So the button said "Open the dashboard" and opened a page that
 * wasn't one, with Wi-Fi setup a further tap down in landing's footer.
 * Removing this page's own picker (2026-07-15) was argued as "the dashboard's
 * Set-up-Wi-Fi panel already does this" — which quietly assumed a dashboard
 * exists. With no dashboard, send them where the work actually is: /wifi,
 * which carries the same picker as the dashboard since the 2026-07-16
 * consolidation. */
"if(j.dash){d0.href=abs(j.dash);d0.textContent='Open the dashboard'}"
"else{d0.href=AP_BASE+'/wifi';d0.textContent='Set up this rover\\u2019s Wi-Fi'}"
"let v=document.getElementById('venue-go');v.hidden=true;"
"if(j.uplink=='full'){tries=0;"
"go.hidden=false;"
"lede.textContent='This board is online'+(j.ssid?' via '+j.ssid:'')+'. Tap Continue to finish, or open the dashboard now.'"
"}else if(j.uplink=='portal'){"
/* The venue's own captive gate answers for the internet until THIS board's
 * MAC signs in — and one sign-in from here covers it (NAT: every client
 * shares the board's venue-side MAC). Navigating in this very sheet is
 * exactly right for once. */
"go.hidden=true;"
"lede.textContent='This board joined '+(j.ssid||'the venue Wi-Fi')+', but that network asks for its own "
"sign-in before it gives internet. Sign in once below and this board is set for everyone.';"
"v.href=j.portal_url||'http://example.com/';"
"v.textContent='Sign in to '+(j.ssid||'the venue Wi-Fi');"
"v.hidden=false"
"}else if(j.state=='searching'){"
"go.hidden=true;"
"lede.textContent='The board is connecting\\u2026 this takes a few seconds.'"
"}else if(j.ssid&&++tries<7){"
"go.hidden=true;"
"lede.textContent='Reconnecting to '+j.ssid+'\\u2026'"
"}else{"
"go.hidden=true;"
"lede.textContent='This board isn\\u2019t online yet \\u2014 open the dashboard to drive it offline, or set up its Wi-Fi there.'"
"}"
"setTimeout(refresh,3000)"
"}).catch(()=>{"
"lede.textContent='Checking this board\\u2019s status\\u2026';setTimeout(refresh,3000)});}"
"go.addEventListener('click',async()=>{"
"try{await fetch('/captive/ack',{method:'POST'})}catch(e){}"
/* Real navigation, not a DOM swap — see the ?done=1 comment above. */
"location.href='/welcome?done=1'"
"});"
"</script></body></html>";

/* This board's own literal IP, "http://a.b.c.d" — set once at
 * wifi_portal_start() (AP IP is fixed for the boot), spliced into WELCOME's
 * AP_BASE. Empty until then, which resolves to a harmless relative link (the
 * pre-boot window this page is never actually served in). */
static char s_ap_base[24] = "";

static esp_err_t welcome_get(httpd_req_t *req)
{
    char host[64] = "?";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    ESP_LOGI(TAG, "GET /welcome (Host: %s)", host);
    char ap_base_js[48];
    snprintf(ap_base_js, sizeof ap_base_js, "const AP_BASE='%s';", s_ap_base);
    if (send_head(req, NULL) != ESP_OK) return ESP_FAIL;   /* no page-specific CSS */
    if (httpd_resp_send_chunk(req, WELCOME_BODY, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, ap_base_js, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, WELCOME_POST, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);   /* terminates the chunked response */
}

/* GET /captive-portal-api — RFC 8908, what the DHCP option 114 URI
 * (hub_role.c's wifi_apsta_up) points at. The modern replacement for the OS
 * guessing captivity from a probe redirect: iOS 14+/macOS Big Sur+/Android
 * 11+ can fetch this directly from the DHCP offer and skip the heuristic
 * entirely. Apple's spec wants this served over TLS — no real cert exists
 * for a private classroom AP, so a strict client may simply decline to use
 * it; the probe-redirect flow above still covers those. */
static esp_err_t captive_api_get(httpd_req_t *req)
{
    char body[144];
    if (captive_accepted(client_ip(req))) {
        snprintf(body, sizeof body, "{\"captive\":false}");
    } else {
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip_info = {0};
        if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
            snprintf(body, sizeof body, "{\"captive\":true,\"user-portal-url\":\"http://" IPSTR "/welcome\"}",
                     IP2STR(&ip_info.ip));
        } else {
            snprintf(body, sizeof body, "{\"captive\":true,\"user-portal-url\":\"http://192.168.99.1/welcome\"}");
        }
    }
    httpd_resp_set_type(req, "application/captive+json");
    return httpd_resp_sendstr(req, body);
}

/* OS-native captive-portal connectivity probes (robot's own island onboarding,
 * NOT the classroom/MDM auto-join flow — CLAUDE.md § Status & design history).
 * Apple, Android, and Windows each fire a GET against one of these fixed,
 * well-known paths right after a device joins any network. An unacked client
 * redirects to /welcome, above — dashboard-free, so a captive sheet never
 * renders the real page. An acked client (tapped Continue there) gets each
 * OS's genuine success signature, the only thing that makes the OS actually
 * dismiss its own captive-portal sheet. The wildcard DNS responder
 * (dns_server.c) is what makes these probes reach this board in the first
 * place, for any hostname the OS queries.
 *
 * Strictly additive: a device that never fires a probe (locked-down MDM
 * policy, or an OS/version that skips it) sees no different behavior than
 * today — it still just manually visits rover.local or /wifi. */
/* Absolute, IP-literal portal URL for every redirect Location (built once at
 * portal start — the AP IP is fixed for the boot). Implementer consensus:
 * never redirect a walled-garden client to a hostname — Android's login
 * WebView can't resolve .local, and DNS inside the garden is the top
 * cross-OS failure mode. */
static char s_welcome_url[48] = "/welcome";

static esp_err_t probe_redirect(httpd_req_t *req)
{
    char host[64] = "?";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    bool accepted = captive_accepted(client_ip(req));
    ESP_LOGI(TAG, "probe %s (Host: %s) -> %s", req->uri, host,
             accepted ? "genuine success" : "302 /welcome");
    if (!accepted) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", s_welcome_url);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (strcmp(req->uri, "/generate_204") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    if (strcmp(req->uri, "/connecttest.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft Connect Test");
    }
    if (strcmp(req->uri, "/ncsi.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft NCSI");
    }
    /* Apple's hotspot-detect.html: the CNA checks the body for this exact string. */
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

/* Catch-all 404: the four probe routes above are only the paths we KNOW; the
 * wildcard DNS funnels every hostname here, and a probe we don't know by path
 * (Windows /redirect, Firefox canonical.html, old-Apple /library/test/…,
 * OEM variants) would otherwise 404 — which reads as "no internet", NOT
 * "captive", so no sheet ever opens on those clients. Rule: a 404 whose Host
 * is a public DNS name is somebody's probe → 302 to the portal (the ESP-IDF
 * captive_portal example's design). Our own hosts — an IP literal, *.local,
 * or a bare name — keep their honest 404, so the dashboard's/IDE's own API
 * misses never bounce to /welcome. */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    bool dotted = false, ip_literal = true;
    for (const char *p = host; *p; p++) {
        if (*p == '.') dotted = true;
        if ((*p < '0' || *p > '9') && *p != '.' && *p != ':') ip_literal = false;
    }
    size_t hl = strlen(host);
    bool local = hl >= 6 && strcasecmp(host + hl - 6, ".local") == 0;
    if (!dotted || ip_literal || local) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "not found");
    }
    ESP_LOGI(TAG, "404 %s (Host: %s) -> 302 %s (unlisted probe path)", req->uri, host, s_welcome_url);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_welcome_url);
    return httpd_resp_send(req, NULL, 0);
}

void wifi_portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    /* 5 (was 3, was 7): this handle also serves the dashboard + IDE once
     * start_ws_mqtt_bridge registers onto it, and the chip-wide LWIP pool
     * (24, sdkconfig LWIP_MAX_SOCKETS) is shared with mosquitto/rovers/DNS/
     * mDNS — a 7 budget let one IDE page load starve the broker's accept
     * loop (2026-07-13), while 3 made Chrome's keep-alive connections 503
     * the page's own /wifi/status poll (Duke bench 2026-07-14). The IDE
     * bundle is built to load within this (ide-v7 script concat). */
    cfg.max_open_sockets = 5;
    /* True peak on THIS shared handle:
     *   /wifi{,/scan,/save,/connect,/forget,/role×2,/status,/professor}
     *   + / + /welcome + /captive/ack + /captive-portal-api
     *   + the 4 captive-portal probe paths below
     *   = 17 registered right after this function returns.
     * start_ws_mqtt_bridge then drops / (-1) and adds / (dashboard) + /fleet
     * + /ide/?* (+3) — its ws_root/ws_mqtt live on a SEPARATE httpd (ws_srv),
     * so they don't count here — = 19 peak. +1 headroom over that measured
     * peak, not a round-number guess.
     * This is a COUNTED budget: adding a route without bumping it silently
     * costs the last one registered (/wifi/professor took it from 18 to 19 on
     * 2026-07-16, which would have left zero headroom at 19). */
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;
    /* Wildcard matcher for the bridge's /ide/?* route (ws_mqtt_bridge.c
     * registers onto this shared handle); URIs without '*' — everything
     * below — still match exactly as before. */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "config panel httpd (:80) failed to start");
        s_http = NULL;
        return;
    }
    /* The AP IP is fixed for the boot — bake the absolute portal URL every
     * probe/catch-all redirect points at (falls back to the relative path),
     * and s_ap_base for WELCOME's own dashboard link (same reasoning). */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ipi = {0};
    if (ap && esp_netif_get_ip_info(ap, &ipi) == ESP_OK) {
        snprintf(s_welcome_url, sizeof s_welcome_url, "http://" IPSTR "/welcome", IP2STR(&ipi.ip));
        snprintf(s_ap_base, sizeof s_ap_base, "http://" IPSTR, IP2STR(&ipi.ip));
    }
    httpd_uri_t u_page  = { .uri = "/wifi",        .method = HTTP_GET,  .handler = page_get };
    httpd_uri_t u_scan  = { .uri = "/wifi/scan",   .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t u_save  = { .uri = "/wifi/save",    .method = HTTP_POST, .handler = save_post };
    httpd_uri_t u_conn  = { .uri = "/wifi/connect", .method = HTTP_POST, .handler = connect_post };
    httpd_uri_t u_forget = { .uri = "/wifi/forget", .method = HTTP_POST, .handler = forget_post };
    httpd_uri_t u_rget  = { .uri = "/wifi/role",   .method = HTTP_GET,  .handler = role_get };
    httpd_uri_t u_rpost = { .uri = "/wifi/role",   .method = HTTP_POST, .handler = role_post };
    httpd_uri_t u_stat  = { .uri = "/wifi/status", .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t u_prof  = { .uri = "/wifi/professor", .method = HTTP_POST, .handler = professor_post };
    httpd_uri_t u_root  = { .uri = "/",            .method = HTTP_GET,  .handler = landing_get };
    /* The captive-portal Accept flip — see probe_redirect's comment above. */
    httpd_uri_t u_welcome = { .uri = "/welcome",     .method = HTTP_GET,  .handler = welcome_get };
    httpd_uri_t u_ack     = { .uri = "/captive/ack", .method = HTTP_POST, .handler = captive_ack_post };
    /* RFC 8908 — the URI hub_role.c's DHCP option 114 advertises. */
    httpd_uri_t u_capi    = { .uri = "/captive-portal-api", .method = HTTP_GET, .handler = captive_api_get };
    /* OS captive-portal probes — see probe_redirect's comment above. */
    httpd_uri_t u_apple   = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = probe_redirect };
    httpd_uri_t u_android = { .uri = "/generate_204",        .method = HTTP_GET, .handler = probe_redirect };
    httpd_uri_t u_wintest = { .uri = "/connecttest.txt",     .method = HTTP_GET, .handler = probe_redirect };
    /* Windows checks BOTH ncsi.txt (content match) and connecttest.txt
     * (redirect/status); its auto-open behavior across versions is less
     * consistent than Apple's/Android's — sometimes it's only a taskbar toast,
     * not a full popup — so this is best-effort, not a guaranteed auto-open. */
    httpd_uri_t u_ncsi    = { .uri = "/ncsi.txt",             .method = HTTP_GET, .handler = probe_redirect };
    httpd_register_uri_handler(s_http, &u_page);
    httpd_register_uri_handler(s_http, &u_scan);
    httpd_register_uri_handler(s_http, &u_save);
    httpd_register_uri_handler(s_http, &u_conn);
    httpd_register_uri_handler(s_http, &u_forget);
    httpd_register_uri_handler(s_http, &u_rget);
    httpd_register_uri_handler(s_http, &u_rpost);
    httpd_register_uri_handler(s_http, &u_stat);
    httpd_register_uri_handler(s_http, &u_prof);
    httpd_register_uri_handler(s_http, &u_root);
    httpd_register_uri_handler(s_http, &u_welcome);
    httpd_register_uri_handler(s_http, &u_ack);
    httpd_register_uri_handler(s_http, &u_capi);
    httpd_register_uri_handler(s_http, &u_apple);
    httpd_register_uri_handler(s_http, &u_android);
    httpd_register_uri_handler(s_http, &u_wintest);
    httpd_register_uri_handler(s_http, &u_ncsi);
    /* Unlisted probe paths on hijacked hostnames — see not_found_handler. */
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, not_found_handler);

    /* Wildcard :53 responder so the probes above actually get dialed at this
     * board (a joining device has no other DNS to ask on this AP anyway). One
     * task for the life of the boot, same lifetime as this httpd. */
    dns_server_start();

    ESP_LOGI(TAG, "config panel on :80 (/wifi + /wifi/status; state-routing landing at /; "
                  "captive-portal probes -> /welcome until Continue is tapped)");
}

httpd_handle_t wifi_portal_httpd(void)
{
    return s_http;
}
