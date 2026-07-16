# Unified rover image: dispatcher (main.c) + the two boot roles. The rover role
# is the esp-mqtt client + motor drive; the hub role is the on-chip broker + WS
# bridge (mosquitto + mdns come from idf_component.yml, which auto-adds their
# REQUIRES). esp_http_server + lwip are the hub role's httpd/bridge deps.
# dashboard_html.c is GENERATED (tools/embed_dashboard.py, run pre-build via
# platformio.ini) — the hub role's embedded dashboard as a plain byte array.
# We compile it as a normal source instead of an objcopy/.S embed because
# PlatformIO's SCons build doesn't wire a generated .S into the link. The
# generator writes it from web/dashboard.html (VENDORED, canonical in
# better-robotics/hub; resync via tools/sync-dashboard.sh).
idf_component_register(SRCS "main.c" "rover_role.c" "hub_role.c" "ws_mqtt_bridge.c"
                             "wifi_portal.c" "dns_server.c" "camera.c" "dashboard_html.c"
                             "ota_update.c" "provisioning_util.c" "rover_config.c" "captive_nat.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer
                                driver mqtt json esp_http_server lwip
                                app_update mbedtls)
