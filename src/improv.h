#ifndef ROVER_IMPROV_H
#define ROVER_IMPROV_H
#include <stddef.h>
#include <stdint.h>

// GATT UUIDs (string form) — verbatim from hub/src/improv.rs.
#define IMPROV_SERVICE_UUID   "00467768-6228-2272-4663-277478268000"
#define IMPROV_CHAR_STATE     "00467768-6228-2272-4663-277478268001"
#define IMPROV_CHAR_ERROR     "00467768-6228-2272-4663-277478268002"
#define IMPROV_CHAR_RPC_CMD   "00467768-6228-2272-4663-277478268003"
#define IMPROV_CHAR_RPC_RES   "00467768-6228-2272-4663-277478268004"
#define IMPROV_CHAR_CAPS      "00467768-6228-2272-4663-277478268005"

enum { IMPROV_STATE_AUTH_REQUIRED=0x01, IMPROV_STATE_AUTHORIZED=0x02,
       IMPROV_STATE_PROVISIONING=0x03, IMPROV_STATE_PROVISIONED=0x04 };
enum { IMPROV_ERR_NONE=0x00, IMPROV_ERR_INVALID_PKT=0x01, IMPROV_ERR_UNKNOWN_RPC=0x02,
       IMPROV_ERR_UNABLE_CONNECT=0x03, IMPROV_ERR_NOT_AUTHORIZED=0x04, IMPROV_ERR_UNKNOWN=0xFF };
enum { IMPROV_CMD_SEND_WIFI=0x01, IMPROV_CMD_IDENTIFY=0x02,
       IMPROV_CMD_DEVICE_INFO=0x03, IMPROV_CMD_SCAN_WIFI=0x04 };
#define IMPROV_CAP_IDENTIFY  0x01
#define IMPROV_CAP_SCAN_WIFI 0x04

typedef enum { IMPROV_OK, IMPROV_ERR_INVALID, IMPROV_ERR_UNKNOWN_CMD } improv_parse_t;
typedef struct { uint8_t cmd; char ssid[33]; char pass[65]; } improv_command_t;

uint8_t improv_checksum(const uint8_t *b, size_t n);
improv_parse_t improv_parse_command(const uint8_t *pkt, size_t n, improv_command_t *out);
size_t improv_encode_result(uint8_t cmd, const char *const *strings, size_t count,
                            uint8_t *out, size_t cap);
#endif
