/*
 * wifi_portal.c — the per-board Wi-Fi config panel (robot#2 / #17).
 *
 * One always-on httpd on the board's :80, started by board_run right after the AP
 * comes up — before any broker decision, so it is already serving whatever the
 * board turns out to be.
 *
 * Its job is the ISLAND board: no hub, so no dashboard until it self-brokers, and
 * a home uplink that cannot be configured over the network it hasn't joined. The
 * student joins the open robot-<id> AP, browses robot.local, sets home Wi-Fi.
 *
 * A HUB-JOINED board is the other case, and it needs almost none of this: it has
 * no AP at all (hub_role.c board_ap_down — one network in the room, the hub's),
 * takes its name and pins over MQTT, and drives off the hub's dashboard. This
 * httpd stays up for it, reachable at robot-<id>.local on the hub's LAN, but
 * nothing routine sends anyone here.
 *
 * The scan itself lives in hub_role.c (board_wifi_scan) because that file owns the
 * radio and the s_want_connect reconnect gate; this file is only presentation +
 * NVS + the config-apply reboot.
 */
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>         /* json_append — bounded vsnprintf into a budget */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"       /* esp_timer_get_time — the captive-ack idle window */
#include "esp_netif.h"       /* the AP's own IP, for the RFC 8908 captive-portal-api body */
#include "esp_wifi.h"        /* esp_wifi_ap_get_sta_list — the presence reaper's station list */
#include "esp_wifi_ap_get_sta_list.h"   /* ...with_ip: MAC+IP per station, keyed as s_accepted is */
#include "cJSON.h"          /* /wifi/connect speaks hubd's JSON dialect */
#include "roles.h"          /* board_ap_t, board_wifi_scan */
#include "robot_config.h"   /* robot_config_set_wifi, robot_config_load */
#include "provisioning_util.h" /* robot_host_is_local — captive 404 + rebind guard */
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
".warn{color:var(--warn-ink);font-weight:600}"
/* Advanced disclosure: role + operator password fold away so the Wi-Fi task
 * is the whole page for a student. A <details class=card> — the summary is the
 * card heading, the two controls inside get smaller sub-headings so the
 * hierarchy reads section > item. Collapsed by default. */
"summary{cursor:pointer;font-size:20px;font-weight:700;letter-spacing:-.015em;"
"list-style:none;display:flex;justify-content:space-between;align-items:center}"
"summary::-webkit-details-marker{display:none}"
"summary::after{content:'\\203a';color:var(--ink-muted);font-weight:400;transition:transform .2s}"
"details[open] summary::after{transform:rotate(90deg)}"
"details.card h2{font-size:15px;font-weight:600;margin:14px 0 4px}";

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
"<h2>Set the robot&rsquo;s Wi-Fi</h2>"
"<p class=s>Pick your home network and the robot restarts to join it.</p>"
/* No Scan button: opening the page IS the scan request, same as the dashboard
 * picker and as iPhone/Android. Rescan is a quiet link in .foot. */
"<div id=nets class=list-group></div>"
"<div id=join hidden><strong id=jssid></strong>"
"<input id=pass type=password placeholder=\"Network password\" autocapitalize=off autocorrect=off>"
"<button id=go class=btn-primary>Connect</button></div>"
"<div id=msg class=s></div>"
"<p class=foot><button id=rescan class=link-btn>Rescan</button></p>"
"</div>"
/* The default-password warning lives ABOVE the fold, not inside it: the field
 * it points to is collapsed under Advanced, but this alert has to stay visible
 * on any board still on the built-in password — that credential gates OTA on
 * EVERY board, so a silent default is a whole-fleet reflash risk. setprofwarn
 * clears it once a real one is stored. */
"<p class=\"s warn\" id=profwarn hidden>&#9888; Still the built-in operator password &#8212; anyone on this Wi-Fi can stop the whole class or reflash this board. Set one under Advanced below.</p>"
"<details class=card id=adv>"
"<summary>Advanced</summary>"
"<h2>Board role</h2>"
"<p class=s>What this board is. Changing it restarts the board.</p>"
"<select id=role>"
"<option value=auto>Normal robot &#8212; drives; hosts itself at home</option>"
"<option value=hub>Classroom hub &#8212; hosts the class, no driving</option>"
"<option value=robot>Robot only &#8212; always joins a hub</option>"
"</select>"
"<button onclick=setrole() class=btn>Apply role &amp; restart</button>"
"<div id=rmsg class=s></div>"
/* Operator password. EVERY board checks it now — a hub's broker at
 * connect_cb, and any board's POST /ota (ota_update.c) — so the field renders
 * regardless of role; see the fetch below for what that cost when it didn't.
 * NVS, not a build flag: the compile-time literal is plaintext in the image and
 * firmware.yml publishes .bins from a public repo, so baking a real classroom's
 * password in would ship it to every downloader. Blank = keep whatever is
 * stored (so the field can render without leaking it); "-" clears back to the
 * compile-time default. */
"<div id=profwrap hidden>"
"<h2>Operator password</h2>"
"<p class=s id=profnote>Gates the fleet-wide emergency stop and firmware updates over Wi-Fi. Leave blank to keep the current one; &quot;-&quot; resets it to the built-in default.</p>"
"<input id=profpass type=password placeholder=\"new operator password\" autocapitalize=off autocorrect=off>"
"<button onclick=setprof() class=btn>Save password</button>"
"<div id=pmsg class=s></div>"
"</div>"
"</details>"
"</main>"
"<script>"
"let chosen=null,scanning=false;"
"const nets=document.getElementById('nets'),join=document.getElementById('join'),"
"pass=document.getElementById('pass'),msg=document.getElementById('msg');"
"const note=t=>{nets.innerHTML='';let p=document.createElement('p');p.className='list-note';"
"p.textContent=t;nets.appendChild(p)};"
/* join is MOVED into the list; park it before any rebuild or it's deleted. */
"function park(){join.hidden=true;document.getElementById('msg').before(join)}"
/* 503 = the radio was busy, not an empty room (roles.h board_wifi_scan). Retry
 * here rather than surface it: hub_watch scans every 20 s and the firmware
 * already waited one out, so the honest move is to keep saying "Scanning…"
 * until we actually looked. This existed as `first tap [], second tap the real
 * list` — the student was the retry loop. */
"async function scan(){if(scanning)return;scanning=true;park();note('Scanning\\u2026');"
"let a;"
"try{"
"for(let i=0;;i++){let r=await fetch('/wifi/scan');"
"if(r.status==503&&i<3){await new Promise(z=>setTimeout(z,2000));continue}"
"if(!r.ok)throw new Error(r.status);"
"a=await r.json();break}"
"}"
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
"msg.textContent='Joined \\u2713 \\u2014 saved; restarting to settle in. The robot stays your "
"Wi-Fi ('+chosen+' is only its internet uplink), but it blips off for ~10 seconds. If your phone "
"doesn\\u2019t re-join the robot\\u2019s Wi-Fi by itself, pick it again in Wi-Fi settings, then "
"reopen robot.local.';setTimeout(()=>location.reload(),12000)}"
"else{msg.textContent='Couldn\\u2019t join: '+(res.error||'check the password and try again.')}"
"});"
/* ONE /wifi/role fetch feeds every role-dependent control — the select's value
 * and whether the password field is still warning. It was two identical
 * fetches, which is how one of them ended up commented "/wifi/status carries
 * role" while calling /wifi/role.
 *
 * The password field is NOT role-gated (2026-07-16). It used to be hub-only,
 * correctly: connect_cb was the only thing that read the password, and only a
 * hub runs a broker, so on a robot the control did nothing. POST /ota changed
 * that — every board registers it and every board checks this same password.
 * Leaving the field hub-only meant a robot's OTA endpoint was gated by a
 * password its own panel would not let you set, so it stayed the compile-time
 * default, which ships in every .bin this public repo publishes: anyone on the
 * Wi-Fi could reflash the fleet. Reusing one credential means every surface
 * that spends it must be able to set it. */
"function setprofwarn(d){document.getElementById('profwarn').hidden=!d}"
"document.getElementById('profwrap').hidden=false;"
"fetch('/wifi/role').then(r=>r.json()).then(j=>{"
"document.getElementById('role').value=j.role;"
"setprofwarn(j.prof_default)}).catch(()=>{});"
/* Status card: poll /wifi/status. In embed mode the panel already sits inside the
 * dashboard, so the "open the dashboard" button is suppressed (it would navigate
 * the modal iframe into a nested dashboard). */
"async function stat(){try{let r=await fetch('/wifi/status');let j=await r.json();"
"document.getElementById('bid').textContent=j.board||'This board';"
"let t=j.role=='hub'?'Classroom hub':j.role=='robot'?'Robot (always joins a hub)':'Robot';"
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
/* Same-tab, no _blank: this page is reachable INSIDE the captive sheet (the
 * /welcome picker links here), where an unreleased device can't complete a
 * _blank hand-off — and the page can't tell in-sheet from a real browser. A
 * same-tab nav to the dashboard works in both; in a real browser it just
 * replaces the settings page, which is fine. Suppressed in embed mode above. */
"let a=document.createElement('a');a.className='btn';a.href=j.dash;a.rel='noopener';"
"a.textContent=j.dash=='/'?'Open the dashboard':'Open the class dashboard';g.appendChild(a)}"
"}catch(e){}setTimeout(stat,5000)}"
"stat();"
"async function setrole(){let rm=document.getElementById('rmsg');rm.textContent='applying\\u2026';"
"let v=document.getElementById('role').value;let f=new URLSearchParams();f.append('role',v);"
"try{await fetch('/wifi/role',{method:'POST',body:f});"
"rm.textContent=v=='hub'?'Now the classroom hub. It restarts and appears as an open hub-\\u2026 network (hub.local); robots auto-join it.':"
"v=='robot'?'Robot-only. Restarting \\u2014 it will keep looking for a hub to join.':"
"'Normal robot. Restarting \\u2014 stay on its Wi-Fi and reopen robot.local.'}"
"catch(x){rm.textContent='Applied \\u2014 restarting\\u2026'}}"
"async function setprof(){let m=document.getElementById('pmsg');"
"let v=document.getElementById('profpass').value;"
"if(!v){m.textContent='Enter a password, or \\u2212 to reset to the built-in default.';return}"
"m.textContent='saving\\u2026';"
"let f=new URLSearchParams();f.append('pass',v);"
"try{let r=await(await fetch('/wifi/operator',{method:'POST',body:f})).json();"
"m.textContent=r.ok?(v=='-'?'Reset to the built-in default. New connections use it immediately.':"
"'Saved. New operator connections use it immediately \\u2014 no restart needed.'):"
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
 * reload once THIS board serves it (start_ws_zenoh_bridge takes over "/", so the
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
"document.getElementById('bn').textContent=j.board||'robot';"
"let st=document.getElementById('st'),act=document.getElementById('act');"
/* This board serves the dashboard now — "/" was re-registered, so reload IS it. */
"if(j.dash=='/'){location.reload();return}"
"if(j.dash){"
"st.textContent='Part of the classroom network'+(j.ssid?' '+j.ssid:'')+' \\u2014 drive it from the class dashboard.';"
/* DOM, not innerHTML: dash derives from a stored locator (user-supplied NVS).
 * Same-tab, no _blank — this landing is reachable inside the captive sheet too,
 * where an unreleased device can't complete a _blank hand-off; a same-tab nav
 * works whether we're in the sheet or a real browser. */
"if(!act.firstChild){let a=document.createElement('a');a.className='btn btn-primary';a.href=j.dash;a.rel='noopener';"
"a.textContent='Open the class dashboard';act.appendChild(a)}"
"setTimeout(go,5000);return}"
"act.innerHTML='';"
"st.textContent=n>12?'No hub found yet \\u2014 the robot keeps looking. You can set its Wi-Fi or role below.':"
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

/* Append into a budget, advancing only by what was WRITTEN. `off += snprintf(...)`
 * is the trap this exists to close: snprintf returns the length it WANTED, so a
 * single row over budget pushes off past cap, and the next call's `cap - off`
 * underflows (size_t) to ~SIZE_MAX — an unbounded write at json + off, off the
 * end of the block. Refusing to advance keeps a too-small budget a truncation
 * instead of a heap overflow. */
static bool json_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    if (*off >= cap) return false;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - *off) return false;   /* would truncate — don't advance */
    *off += (size_t)w;
    return true;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    board_ap_t *aps = malloc(SCAN_MAX * sizeof *aps);
    if (!aps) { httpd_resp_send_500(req); return ESP_FAIL; }
    int n = board_wifi_scan(aps, SCAN_MAX);
    /* Busy radio (-1) is NOT an empty room (0) — see roles.h. 503 + Retry-After
     * so the picker can quietly try again instead of publishing "No networks
     * found." as if it had looked. */
    if (n < 0) {
        free(aps);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "2");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"radio busy\"}");
    }

    /* Each row ~ {"ssid":"<=32","signal":100,"security":"WPA2"}, — budget
     * generously. The shape is the Pi hubd's /wifi/scan dialect (wifi.rs
     * scan/map_auth) — the shared dashboard reads `signal` (0-100) and gates
     * the password box on `security === "NO"`; the old {rssi, open} keys
     * rendered as "undefined%" and asked open networks for a password.
     *
     * 112/row is MEASURED against the worst case, not eyeballed — the previous
     * 96 was under it, which is how this became remotely exploitable. esc[]
     * below emits up to 64 bytes (a 32-char SSID of all '"' escapes 1:2), so
     * the longest row is
     *   ,{"ssid":"<64>","signal":100,"security":"WPA2"}
     *   1 + 9 + 64 + 11 + 3 + 13 + 4 + 2 = 107
     * and the budget went negative at n >= 3 — three quote-filled SSIDs from
     * any neighbouring radio, against a /wifi/scan that takes no auth (:1107).
     * The 32 covers "[", "]", the NUL, and the slack that keeps json_append's
     * refusal path unreachable rather than merely survivable. Recount both
     * numbers if a key is renamed or a value's width changes. */
    size_t cap = 32 + (size_t)n * 112;
    char *json = malloc(cap);
    if (!json) { free(aps); httpd_resp_send_500(req); return ESP_FAIL; }
    size_t off = 0;
    json_append(json, cap, &off, "[");
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
        if (!json_append(json, cap, &off, "%s{\"ssid\":\"%s\",\"signal\":%d,\"security\":\"%s\"}",
                         i ? "," : "", esc, pct, aps[i].open ? "NO" : "WPA2")) break;
    }
    json_append(json, cap, &off, "]");

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

/* DNS-rebinding guard for state-changing POSTs. Our wildcard responder answers
 * every name with the AP IP, so a page a joined victim loads from a public site
 * can resolve that site to us and script a POST at our config endpoints. Only an
 * request whose Host is one of our own origins may reconfigure the board; a
 * public Host is a rebind → 403. Reuses the captive 404's discriminator, so the
 * two can't disagree. Returns true when it has already answered (caller returns
 * immediately). */
static bool reject_cross_origin(httpd_req_t *req)
{
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    if (robot_host_is_local(host)) return false;
    ESP_LOGW(TAG, "cross-origin POST %s (Host: %s) refused — DNS-rebind guard", req->uri, host);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "cross-origin request refused");
    return true;
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (reject_cross_origin(req)) return ESP_OK;
    char body[256];
    read_form_body(req, body, sizeof body);

    char ssid[33], pass[65];
    if (!form_field(body, "ssid", ssid, sizeof ssid) || !ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing ssid");
    }
    form_field(body, "pass", pass, sizeof pass);   /* absent → open network */

    esp_err_t e = robot_config_set_wifi(ssid, pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "robot_config_set_wifi failed: %s", esp_err_to_name(e));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved home Wi-Fi '%s' — restarting to join it", ssid);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "saved");
    /* A config-apply reboot: the loop reads NVS only at the top of a pass but sits
     * blocked driving, so a restart is the simplest correct way to re-dial the new
     * network. NOT the deleted mode-switch reboot — always-APSTA comes right back up. */
    board_schedule_reboot("config-apply restart (new Wi-Fi or role)");
    return ESP_OK;
}

/* POST /wifi/forget — "Forget this network" (dashboard's Set-up-Wi-Fi panel).
 * No body needed: erases the stored uplink only (robot_config_clear_wifi) and
 * reboots, same config-apply path as save_post. The board comes back up with
 * no venue/home network configured — a fresh island, same as never-provisioned. */
static esp_err_t forget_post(httpd_req_t *req)
{
    if (reject_cross_origin(req)) return ESP_OK;
    esp_err_t e = robot_config_clear_wifi();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "robot_config_clear_wifi failed: %s", esp_err_to_name(e));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "forgot the stored uplink — restarting as a fresh island");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "forgotten");
    board_schedule_reboot("config-apply restart (new Wi-Fi or role)");
    return ESP_OK;
}

/* POST /wifi/connect — hubd's JSON dialect: body {ssid, password}, reply
 * {"ok":true} / {"ok":false,"error":…}. The shared dashboard's Set-up-Wi-Fi
 * panel drives THIS path on both hubs (dashboard.html wifi-connect); the
 * form-POST /wifi/save above stays as the portal page's own submit.
 * ok now means VERIFIED, matching the Pi hubd (whose nmcli connect blocks
 * until the join lands): the dedicated hub re-dials live and lets its panel
 * watch the join; board/robot mode trial-joins first — the AP and this page
 * stay up through the attempt — and only a verified join is persisted to
 * NVS and config-apply rebooted. A failed trial replies with the verdict
 * ("wrong password?") and saves nothing, so a typo can no longer be saved
 * and rebooted into. */
static esp_err_t connect_post(httpd_req_t *req)
{
    if (reject_cross_origin(req)) return ESP_OK;
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
        if (robot_config_set_wifi(ssid, pass) != ESP_OK)
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not save credentials\"}");
        ESP_LOGI(TAG, "saved uplink '%s' via /wifi/connect — re-dialing live", ssid);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    /* Board/robot mode: the apply is still a config-apply reboot, but the
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
    if (robot_config_set_wifi(ssid, pass) != ESP_OK)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not save credentials\"}");
    ESP_LOGI(TAG, "uplink '%s' verified via /wifi/connect — saving + restarting to settle in", ssid);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    board_schedule_reboot("config-apply restart (new Wi-Fi or role)");
    return ESP_OK;
}

/* ── Board role (#2 tier-2 designate) ─────────────────────────────────────────
 * role_pref selects the boot dispatch (main.c): auto/robot → board_run, hub →
 * hub_role_run. Exposing it here is what lets a operator turn any board into the
 * classroom hub — and back — without reflashing. The hub ALSO serves this panel
 * (hub_role_run calls wifi_portal_start), so designating HUB isn't a one-way trip. */
static const char *role_str(robot_role_pref_t r)
{
    return r == ROLE_HUB ? "hub" : r == ROLE_ROBOT ? "robot" : "auto";
}

static esp_err_t role_get(httpd_req_t *req)
{
    /* prof_default mirrors board_operator_pass_ok's own test (`nvs_pass[0] ?
     * nvs : baked`): unset NVS means this board still admits the compile-time
     * OPERATOR_PASS, which ships in every published .bin — i.e. Stop-all AND
     * POST /ota are gated by a public string. Computed for every role, not just
     * hub: since /ota, a robot has something worth gating too. Discloses
     * nothing — anyone on the open AP can establish the same fact with one
     * CONNECT, or one /ota probe. Told here, not polled from /wifi/status: it
     * changes on a save, never on its own. */
    char nvs_pass[65];
    robot_config_load_operator_pass(nvs_pass);
    char j[64];
    snprintf(j, sizeof j, "{\"role\":\"%s\",\"prof_default\":%s}",
             role_str(robot_config_load_role_pref()),
             nvs_pass[0] ? "false" : "true");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

static esp_err_t role_post(httpd_req_t *req)
{
    if (reject_cross_origin(req)) return ESP_OK;
    char body[64];
    read_form_body(req, body, sizeof body);

    char role[8];
    if (!form_field(body, "role", role, sizeof role)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing role");
    }
    robot_role_pref_t rp = strcmp(role, "hub")   == 0 ? ROLE_HUB
                         : strcmp(role, "robot") == 0 ? ROLE_ROBOT
                         : ROLE_AUTO;   /* unknown → the safe default */
    esp_err_t e = robot_config_set_role_pref(rp);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "robot_config_set_role_pref failed: %s", esp_err_to_name(e));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "role set to '%s' — restarting into the new dispatch", role_str(rp));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    board_schedule_reboot("config-apply restart (new Wi-Fi or role)");
    return ESP_OK;
}

/* POST /wifi/operator — body `pass=<value>`. Sets the hub role's operator
 * password in NVS (robot_config_set_operator_pass), which connect_cb reads
 * per-connection, so a rotation takes effect on the next connect with no
 * reboot. "-" clears back to the compile-time OPERATOR_PASS.
 *
 * NVS and not a build flag: OPERATOR_PASS is a plaintext literal in the
 * image, `robot` is a public repo, and firmware.yml uploads flashable .bins —
 * a baked-in secret ships to everyone who downloads one, and `strings
 * firmware.bin` reads it out. This keeps a real classroom's password off the
 * shared image and per-board.
 *
 * Never echoes the stored value back: the page can set it, not read it. */
static esp_err_t operator_post(httpd_req_t *req)
{
    if (reject_cross_origin(req)) return ESP_OK;
    char body[128];
    read_form_body(req, body, sizeof body);
    char pass[80];
    httpd_resp_set_type(req, "application/json");
    if (!form_field(body, "pass", pass, sizeof pass) || !pass[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing password\"}");
    }
    /* "-" is the clear sentinel, mirroring cmd/config's hub-pin convention.
       An empty stored secret would admit EVERY client as operator, so
       robot_config_set_operator_pass("") erases instead of storing. */
    const bool clearing = strcmp(pass, "-") == 0;
    esp_err_t e = robot_config_set_operator_pass(clearing ? "" : pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set operator pass failed: %s", esp_err_to_name(e));
        return httpd_resp_sendstr(req,
            e == ESP_ERR_INVALID_ARG ? "{\"ok\":false,\"error\":\"too long (max 64)\"}"
                                     : "{\"ok\":false,\"error\":\"could not save\"}");
    }
    /* Never log the value. */
    ESP_LOGW(TAG, "operator password %s", clearing ? "reset to the built-in default" : "changed");
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
 * island robot, wrong for one being passed between students in a classroom.
 * Root cause (documented ESP-IDF quirk, espressif/esp-idf#4863): this httpd's
 * listener is IPv6 internally even for IPv4 connections, so a sockaddr_in
 * read comes back zeroed. The fix is lwip_getpeername() (not the newlib
 * getpeername() wrapper) into a sockaddr_in6 — the real IPv4 word lives at
 * sin6_addr.un.u32_addr[3] (client_ip(), below). ACCEPTED_MAX matches
 * AP_MAX_CONN (hub_role.c) — never more concurrent clients than the AP
 * itself allows; a full table evicts the oldest accept.
 *
 * NOT gated on board_has_uplink() (reversed 2026-07-17 to match the Pi, which
 * releases on Accept regardless of uplink). An Accept returns "genuine success"
 * even on a pure-island board with no uplink — which IS iOS's signal to trust
 * this Wi-Fi and dismiss the sheet, and dismissing is exactly what lets the
 * dashboard then open in the phone's REAL browser offline (target=_blank only
 * hands off once the OS trusts the network). The known cost, taken on purpose:
 * the phone believes this dead network has internet and won't fall back to
 * cellular until it leaves — "connected, but only the dashboard loads".
 * Acceptable for a join-to-drive board, and self-healing: the presence reaper
 * forgets a departed device after 90 s, so its next join is greeted fresh. The
 * 2026-07-14 gate that refused this lie (to keep a phone's cellular fallback
 * honest) is the behaviour we deliberately traded away for the Pi's. */
#define ACKED_IDLE_US (15LL * 60 * 1000 * 1000)
#define ACCEPTED_MAX 8   /* == AP_MAX_CONN, hub_role.c */
static struct { uint32_t ip; int64_t accepted_us; int64_t last_seen_us; } s_accepted[ACCEPTED_MAX];

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

bool captive_accepted(uint32_t ip)   /* exported: captive_nat.c gates its packet
                                        backstop on the same per-device Accept */
{
    if (!ip) return false;
    for (int i = 0; i < ACCEPTED_MAX; i++) {
        if (s_accepted[i].ip != ip) continue;
        /* Measured from LAST SEEN, not accept time: the presence reaper refreshes
         * last_seen_us every poll while the device is associated, so a student who
         * stays connected is never re-greeted, however long they drive. This is
         * only a backstop for when the reaper can't run (no AP station list) —
         * the reaper itself forgets a departed device far sooner (90 s). */
        if (esp_timer_get_time() - s_accepted[i].last_seen_us > ACKED_IDLE_US) {
            s_accepted[i].ip = 0;
            return false;
        }
        return true;   /* released regardless of uplink — the Pi-matching flip, see header */
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
        s_accepted[slot].last_seen_us = s_accepted[slot].accepted_us;
    }
    esp_ip4_addr_t logip = { .addr = ip };
    ESP_LOGI(TAG, "captive/ack from " IPSTR " — probes now answer genuine success", IP2STR(&logip));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

/* Presence reaper — the ESP counterpart to hub/pi/src/bin/hubd.rs's reap_acks,
 * so an Accept means "this visit", not "this device for the next 15 minutes". A
 * device gone from the AP longer than the grace window loses its ack, so its
 * NEXT join is greeted with /welcome again instead of being waved straight
 * through — the behaviour a passed-around classroom board wants, and the reason
 * the Pi's captive greets every fresh connection. Keyed on association presence
 * via esp_netif_get_sta_list, which returns the IP per station — exactly what
 * s_accepted keys on, so no MAC round-trip. Driven by the uplink-probe loop
 * (hub_role.c). GRACE is long enough that a transient re-association blip can't
 * re-summon the sheet mid-drive, short enough that the next student is greeted
 * (matches the Pi's 90 s). If the AP station list can't be read at all — no AP
 * up, e.g. a hub-joined board — it forgets NO ONE: "don't know" is not "nobody
 * is here", the same guarantee the Pi makes on a station-list read failure. */
#define CAPTIVE_ABSENT_GRACE_US (90LL * 1000 * 1000)
void captive_reap_absent(void)
{
    wifi_sta_list_t wsl;
    wifi_sta_mac_ip_list_t nsl;
    if (esp_wifi_ap_get_sta_list(&wsl) != ESP_OK) return;     /* no AP up → forget no one */
    if (esp_wifi_ap_get_sta_list_with_ip(&wsl, &nsl) != ESP_OK) return;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < ACCEPTED_MAX; i++) {
        if (!s_accepted[i].ip) continue;
        bool present = false;
        for (int j = 0; j < nsl.num; j++) {
            if (nsl.sta[j].ip.addr == s_accepted[i].ip) { present = true; break; }
        }
        if (present) {
            s_accepted[i].last_seen_us = now;
        } else if (now - s_accepted[i].last_seen_us > CAPTIVE_ABSENT_GRACE_US) {
            esp_ip4_addr_t g = { .addr = s_accepted[i].ip };
            ESP_LOGI(TAG, "captive: forgetting departed device " IPSTR " — its next join is greeted", IP2STR(&g));
            s_accepted[i].ip = 0;
        }
    }
}

/* /welcome — the ONLY page an unacked captive-portal probe ever redirects to.
 * Deliberately dashboard-free itself: reaching the dashboard is this page's one
 * job, and it opens in the phone's real browser (the "Open the dashboard" link
 * is target=_blank) rather than loading inside this page. This page is only
 * ever reached from the board's own AP, which by construction means the board
 * is islanding and its dashboard is up (a hub-joined board has no AP to serve
 * it — hub_role.c board_ap_down), and the AP does not drop on an uplink change
 * either — so the dashboard is already reachable the instant a phone joins;
 * "Open the dashboard" is the first, unconditional thing offered here
 * (2026-07-15 — this used to be the LAST thing offered, behind a full
 * scan/connect wizard that duplicated what the dashboard's own Set-up-Wi-Fi
 * panel already does against these same /wifi/scan + /wifi/connect endpoints).
 * Configuring Wi-Fi, if the student wants internet, happens there now.
 *
 * Continue is a separate, narrower job: it's what calls /captive/ack, which
 * is what lets the OS's OWN captive sheet auto-dismiss (genuine-success
 * probe answers) instead of sitting on a dead Cancel button. That signal
 * doubles as "trust this Wi-Fi for real routing" to the OS, and is offered
 * regardless of the board's own uplink — the 2026-07-14 gate (flip only when the
 * board had a full uplink) was reversed 2026-07-17 to match the Pi; see the
 * ACCEPTED-table note above. The deliberately-accepted cost: a phone trusts a
 * pure-island board's "internet" and won't fall back to cellular until it leaves.
 * Continue only ever helps the sheet close cleanly; it was never the only way to
 * the dashboard. */
/* Split so welcome_get() can splice a runtime AP_BASE between them (the AP's own
 * IP, computed once at wifi_portal_start() — see s_ap_base below). AP_BASE is
 * the literal IP the dashboard link and its printed address must use: dash is a
 * bare "/" in the common case (hub_role.c) and a relative path is useless to a
 * student retyping it into Safari, while a .local name is exactly what a phone
 * on this AP may not resolve. Same "never hand a walled-garden client a
 * hostname" rule s_welcome_url follows for the initial redirect. */
static const char WELCOME_BODY[] =
"<header class=topbar><h1><span class=a>Better</span><span class=b>Robotics</span></h1></header>"
"<main>"
"<div class=card id=before><h2>Welcome</h2>"
"<p class=s id=lede>Checking this board&#8230;</p>"
/* Matches the Pi's captive welcome (hub/pi/src/welcome.html), deliberately: one
 * greeting, one Accept, then the dashboard — no Wi-Fi picker or role/password
 * here. Those moved to the dashboard's own Set-up-Wi-Fi panel (dashboard.html
 * #wifi-panel, over the same /wifi endpoints), so onboarding happens AFTER the
 * student reaches the dashboard, with room to breathe, instead of a settings
 * wall inside a captive sheet. The one branch that stays is the venue-portal
 * sign-in: a board behind a venue's own captive gate needs one in-sheet
 * tap-through to authorize its MAC for the whole class, and that genuinely
 * can't move to the dashboard. */
"<a class=\"btn btn-primary\" id=venue-go hidden>Sign in</a>"
"<button class=\"btn btn-primary\" id=go hidden>Accept &amp; continue</button>"
"</div>"
/* Post-Accept the device is released (captive_accepted, uplink-independent), so
 * the OS trusts the Wi-Fi and target=_blank opens the phone's REAL browser —
 * even offline, still connected to the board. */
"<div class=card id=after hidden><h2>You&#8217;re set</h2>"
"<a class=\"btn btn-primary\" id=dashgo2 target=_blank rel=noopener hidden>Open the dashboard</a></div>"
"</main>"
"<script>";
/* welcome_get() sends "const AP_BASE='http://a.b.c.d';" here, then this. */
static const char WELCOME_POST[] =
"let lede=document.getElementById('lede'),go=document.getElementById('go'),"
"v=document.getElementById('venue-go');"
"function tier(el,p){el.classList.toggle('btn-primary',p)}"
/* AP_BASE is this board's own literal IP — dash is a bare "/" in the common
 * case (hub_role.c), and a .local name a phone on this AP may not resolve. A
 * dash already 'http...' (a hub-joined board's remote dashboard) passes through. */
"function abs(u){return u&&u.indexOf('http')==0?u:AP_BASE+(u||'/')}"
/* ?done=1 is the post-Accept view. Accept REACHES it by a real navigation
 * (location.href), never a DOM swap: the OS re-runs its captive probe only on a
 * full-page load, so an AJAX-only accept leaves the sheet stuck on Cancel with
 * no auto-dismiss (WBA/Purple consensus; the Cisco ISE CNA bug is exactly this).
 * By the time this renders the device is released, so the dashboard link's
 * static target=_blank opens the phone's real browser. */
"if(location.search.indexOf('done')>=0){"
"document.getElementById('before').hidden=true;"
"document.getElementById('after').hidden=false;"
"fetch('/wifi/status').then(r=>r.json()).then(j=>{"
"let g=document.getElementById('dashgo2');g.hidden=!j.dash;g.href=abs(j.dash)"
"}).catch(()=>{})"
"}else{refresh()}"
/* Poll for the ONE state that changes what's offered — a venue portal.
 * Everything else is the same greeting + Accept; Wi-Fi and settings live on the
 * dashboard now (see WELCOME_BODY). Accept is always available (it releases the
 * device regardless of uplink), so even a portal board can just drive. */
"function refresh(){"
"fetch('/wifi/status').then(r=>r.json()).then(j=>{"
"go.hidden=false;"
"if(j.uplink=='portal'){"
/* The board joined a venue whose network has its own captive gate. Behind the
 * NAT every client shares the board's venue-side MAC, so one sign-in from this
 * phone authorizes the board for the whole class — and it has to happen inside
 * this captive session, the one thing that can't move to the dashboard. Sign-in
 * is primary; Accept drops to neutral so a student can still just drive. */
"v.hidden=false;v.href=j.portal_url||'http://example.com/';"
"v.textContent='Sign in to '+(j.ssid||'the Wi-Fi');"
"tier(v,true);tier(go,false);"
"lede.textContent='This board joined '+(j.ssid||'the venue Wi-Fi')+', which wants its own sign-in before it shares internet. Sign in to set up the class, or just Accept to drive.'"
"}else{"
"v.hidden=true;tier(go,true);"
"lede.textContent='You\\u2019re connected to '+(j.board||'this robot')+'.'"
"}"
"setTimeout(refresh,3000)"
"}).catch(()=>{lede.textContent='Checking this board\\u2019s status\\u2026';setTimeout(refresh,3000)})}"
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
 * The genuine-success bodies below are the table in better-robotics/hub's
 * CONTRACT.md § Captive onboarding — the single spec this and the Pi's hubd.rs
 * both reconcile to; keep every row byte-identical across both hubs.
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
 * today — it still just manually visits robot.local or /wifi. */
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
    /* Never let an intermediary or the OS cache the captive verdict: a stale
     * "302 /welcome" would strand a released device, a stale success would
     * skip greeting a fresh one. RFC 8908 says the same for the API half. Set
     * once here — httpd carries it onto whichever branch sends below. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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
    /* Firefox's detectportal probe checks for this EXACT body (lowercase,
     * trailing newline) — the Apple HTML below would leave a released Firefox
     * reading as still-captive, since we keep hijacking the probe name for
     * accepted clients rather than letting it reach the real server. */
    if (strcmp(req->uri, "/success.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "success\n");
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
    if (robot_host_is_local(host)) {   /* our own name → honest 404, not a probe */
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "not found");
    }
    ESP_LOGI(TAG, "404 %s (Host: %s) -> 302 %s (unlisted probe path)", req->uri, host, s_welcome_url);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_welcome_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");   /* don't cache the redirect past a release */
    return httpd_resp_send(req, NULL, 0);
}

void wifi_portal_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    /* 5 (was 3, was 7): this handle also serves the dashboard + the IDE
     * shell once start_ws_zenoh_bridge registers onto it, and the chip-wide
     * LWIP pool (24, sdkconfig LWIP_MAX_SOCKETS) is shared with mosquitto/
     * robots/DNS/mDNS — a 7 budget let one embedded-IDE page load starve the
     * broker's accept loop (2026-07-13), while 3 made Chrome's keep-alive
     * connections 503 the page's own /wifi/status poll (Duke bench
     * 2026-07-14). The IDE shell keeps this safe by construction: ONE request
     * hits this chip, every asset after it goes to GitHub Pages. */
    cfg.max_open_sockets = 5;
    /* True peak on THIS shared handle:
     *   /wifi{,/scan,/save,/connect,/forget,/role×2,/status,/operator}
     *   + / + /welcome + /captive/ack + /captive-portal-api
     *   + the 4 captive-portal probe paths below
     *   = 17 registered right after this function returns.
     * start_ws_zenoh_bridge then drops / (-1) and adds / (dashboard) + /fleet
     * + /ide/ + /ide (+4) — its ws_root/ws_mqtt live on a SEPARATE httpd
     * (ws_srv), so they don't count here — ota_update_start adds POST /ota +
     * OPTIONS /ota (+2, the preflight the dashboard's cross-origin push
     * needs), and device_log_serve adds GET /log (+1)
     * = 24 peak. +1 headroom over that measured peak, not a round-number guess.
     * This is a COUNTED budget: adding a route without bumping it silently
     * costs the last one registered (/wifi/operator took it from 18 to 19 on
     * 2026-07-16, which would have left zero headroom at 19; dropping the IDE's
     * /ide/?* wildcard took it back to 18; /ota, its preflight and /log took it
     * to 21; /success.txt (Firefox probe) took it to 22 on 2026-07-19; the IDE
     * loader shell's /ide/ + /ide took it to 24 on 2026-07-20). */
    cfg.max_uri_handlers = 25;
    cfg.lru_purge_enable = true;
    /* No uri_match_fn: every route here is an exact path. The wildcard matcher
     * was here only for the bridge's /ide/?* route, which left with the IDE
     * bundle (2026-07-16). Query strings split off before matching either way. */

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
    httpd_uri_t u_prof  = { .uri = "/wifi/operator", .method = HTTP_POST, .handler = operator_post };
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
    /* Firefox's detectportal. MUST be registered, not just handled in
     * probe_redirect: an unregistered path falls to not_found_handler, which
     * 302s any foreign Host with no acked check — so a RELEASED Firefox client
     * would be bounced back to /welcome. (The handler arm existed unwired until
     * 2026-07-19; reconciled to the Pi + CONTRACT.md § Captive onboarding.) */
    httpd_uri_t u_firefox = { .uri = "/success.txt",          .method = HTTP_GET, .handler = probe_redirect };
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
    httpd_register_uri_handler(s_http, &u_firefox);
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
