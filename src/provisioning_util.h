#ifndef ROVER_PROVISIONING_UTIL_H
#define ROVER_PROVISIONING_UTIL_H
#include <stdbool.h>
#include <stdint.h>
bool rover_validate_locator(const char *s);
void rover_format_robot_id(const uint8_t mac[6], char out[16]);
#endif
