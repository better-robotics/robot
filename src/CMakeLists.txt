idf_component_register(SRCS "main.c" "improv.c" "provisioning_util.c" "rover_config.c" "provisioning.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer bt driver mqtt)
