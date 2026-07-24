# Unified robot image: dispatcher (main.c) + the two boot roles. The robot role
# is the zenoh-pico drive client + motor drive; the hub role is a zenoh-pico
# peer-listen session + the WS-JSON adapter (ws_zenoh_bridge). zenoh-pico comes
# from platformio.ini lib_deps (not an IDF REQUIRES); mdns comes from
# idf_component.yml. esp_http_server + lwip are the hub role's httpd/adapter deps.
# dashboard_html.c and ide_shell_html.c are GENERATED (tools/embed_web.py, run
# pre-build via platformio.ini) — the hub role's embedded pages as plain byte
# arrays: the full dashboard, and the ~2 KB IDE loader shell served at /ide/.
# We compile them as normal sources instead of an objcopy/.S embed because
# PlatformIO's SCons build doesn't wire a generated .S into the link. The
# generator writes them from web/ (VENDORED: dashboard.html canonical in
# sprocket-robotics/hub, ide_shell.html in sprocket-robotics/ide; resync via
# tools/sync-dashboard.sh / tools/sync-ide-shell.sh).
idf_component_register(SRCS "main.c" "robot_role.c" "hub_role.c" "ws_zenoh_bridge.c"
                             "wifi_portal.c" "dns_server.c" "camera.c" "dashboard_html.c" "ide_shell_html.c"
                             "device_log.c" "ota_update.c" "provisioning_util.c" "robot_config.c" "captive_nat.c"
                             "ws2812.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer
                                driver mqtt json esp_http_server lwip
                                app_update mbedtls)
