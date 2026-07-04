#ifndef ROVER_CONFIG_H
#define ROVER_CONFIG_H
#include <stdbool.h>
#include "esp_err.h"

/* Board identity rides telemetry + Improv device-info as metadata — the
 * robot id stays role-named (rover-XXXX), hardware never enters the name. */
#ifndef HW_BOARD
#define HW_BOARD "esp32"   /* each PlatformIO env passes its own */
#endif

bool rover_config_load(char ssid[33], char pass[65], char locator[65]);
esp_err_t rover_config_set_wifi(const char *ssid, const char *pass);
esp_err_t rover_config_set_locator(const char *locator);
bool rover_config_is_complete(void);

#endif
