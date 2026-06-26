# Rover BLE Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provision the rover's Wi-Fi (stock Improv) and hub locator (one custom GATT characteristic) over BLE, replacing every hardcoded value in `main.c`, with no reflash to change networks.

**Architecture:** Mode-switched NimBLE firmware — BLE provisioning mode when NVS is unconfigured, Wi-Fi operating mode once provisioned; the two radios never run at once. Pure wire/validation logic lives in ESP-free files unit-tested on the host; radio/BLE/Wi-Fi/zenoh integration is verified on hardware. A sibling web page (`better-robotics/provision/rover.html`) drives it.

**Tech Stack:** ESP-IDF via PlatformIO (`espressif32@6.13.0`), NimBLE (ESP-IDF's `bt` component), `zenoh-pico` 1.9.0, NVS, Unity (PlatformIO `native` env) for host tests. Web: vendored Improv SDK + Web Bluetooth.

## Global Constraints

- Platform pinned **`espressif32@6.13.0`** (`<7.x` — 7.0 breaks zenoh-pico's PIO build). Do not bump.
- Board: classic **ESP32-D0WD** (`az-delivery-devkit-v4`), single shared 2.4 GHz radio, 4 MB flash, single-app-large partition.
- **Never run BLE and Wi-Fi concurrently** — mode-switch only. BLE off before Wi-Fi starts; Wi-Fi off before BLE starts.
- **Measured data only** in telemetry (`synthetic:false`) — unchanged from v1.
- Improv wire values are **copied verbatim** from `better-robotics/hub/src/improv.rs` for interop with the stock client. Do not re-derive from memory.
- `hubcfg` locator string format: `tcp/<host>:<port>`, UTF-8, ≤ 64 bytes.
- Telemetry key: `robots/rover-XXXX/sys`, where `rover-XXXX` = last 2 bytes of the ESP32 base MAC, lowercase hex.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` stays (zenoh needs it).
- zenoh-pico API: `z_open` auto-starts read/lease tasks — do NOT call `zp_start_read_task`/`zp_start_lease_task`.

---

## File Structure

**Firmware (`better-robotics/rover/`):**
- `src/improv.h` / `src/improv.c` — **pure** Improv device-side wire protocol (checksum, parse command, encode result). No ESP includes. Native-testable. Port of `hub/src/improv.rs`.
- `src/provisioning_util.h` / `src/provisioning_util.c` — **pure** helpers: `rover_validate_locator()`, `rover_format_robot_id()`. No ESP includes. Native-testable.
- `src/rover_config.h` / `src/rover_config.c` — NVS load/store/clear + `rover_config_is_complete()`. ESP (`nvs`).
- `src/provisioning.h` / `src/provisioning.c` — NimBLE GATT server (Improv + hubcfg services), advertising, write handlers that validate + persist + drive Wi-Fi join. ESP + `bt`.
- `src/main.c` — mode dispatch + operating mode (Wi-Fi STA from NVS → zenoh client → telemetry), re-provisioning fallback.
- `src/CMakeLists.txt` — add sources + `REQUIRES bt`.
- `platformio.ini` — add `[env:native]` for host unit tests.
- `test/test_improv/test_improv.c` — Unity tests for the wire protocol.
- `test/test_util/test_util.c` — Unity tests for locator validation + robot-id formatting.

**Web (`better-robotics/provision/`):**
- `rover.html` — Improv launch button (Wi-Fi) + custom Web-Bluetooth control writing the `hubcfg` locator characteristic. Reuses the already-vendored Improv SDK.

**The `hubcfg` GATT contract (shared constant, both repos):**
- Service UUID: `0000fe07-0000-1000-8000-00805f9b34fb`  *(placeholder base; finalized in Task 5 — a fresh random 128-bit UUID, recorded as the canonical constant in `src/provisioning.c` and `rover.html`)*
- Characteristic `locator`: write-with-response + read; UTF-8 `tcp/<host>:<port>`.

---

### Task 1: Improv wire protocol (pure C port) + native test harness

**Files:**
- Create: `src/improv.h`, `src/improv.c`
- Create: `test/test_improv/test_improv.c`
- Modify: `platformio.ini` (add `[env:native]`)

**Interfaces:**
- Produces:
  - `uint8_t improv_checksum(const uint8_t *b, size_t n)`
  - `typedef enum { IMPROV_OK, IMPROV_ERR_INVALID, IMPROV_ERR_UNKNOWN_CMD } improv_parse_t;`
  - `typedef struct { uint8_t cmd; char ssid[33]; char pass[65]; } improv_command_t;`
  - `improv_parse_t improv_parse_command(const uint8_t *pkt, size_t n, improv_command_t *out);`
  - `size_t improv_encode_result(uint8_t cmd, const char *const *strings, size_t count, uint8_t *out, size_t cap);`
- Constants in `improv.h` copied verbatim from `hub/src/improv.rs`: command ids `IMPROV_CMD_SEND_WIFI=0x01`, `IMPROV_CMD_IDENTIFY=0x02`, `IMPROV_CMD_DEVICE_INFO=0x03`, `IMPROV_CMD_SCAN_WIFI=0x04`; state values `0x01..0x04`; error values `0x00..0x05,0xFF`; cap bits `CAP_IDENTIFY=1`, `CAP_SCAN_WIFI=4`.

- [ ] **Step 1: Add the native test env to `platformio.ini`**

Append:

```ini
[env:native]
platform = native
test_framework = unity
build_flags = -DUNIT_TEST -std=c11
```

- [ ] **Step 2: Write `src/improv.h`**

```c
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
```

- [ ] **Step 3: Write the failing tests `test/test_improv/test_improv.c`**

Mirror the Rust test vectors from `hub/src/improv.rs` so the byte framing is provably identical.

```c
#include <string.h>
#include <unity.h>
#include "improv.h"

void setUp(void) {}
void tearDown(void) {}

static void make_wifi(uint8_t *pkt, size_t *n, const char *ssid, const char *pass) {
    size_t sl = strlen(ssid), pl = strlen(pass);
    uint8_t data[80]; size_t d = 0;
    data[d++] = (uint8_t)sl; memcpy(data+d, ssid, sl); d += sl;
    data[d++] = (uint8_t)pl; memcpy(data+d, pass, pl); d += pl;
    size_t i = 0; pkt[i++] = IMPROV_CMD_SEND_WIFI; pkt[i++] = (uint8_t)d;
    memcpy(pkt+i, data, d); i += d;
    pkt[i] = improv_checksum(pkt, i); i++; *n = i;
}

void test_checksum_low_byte_sum(void) {
    uint8_t a[] = {0x01, 0xFF}; TEST_ASSERT_EQUAL_HEX8(0x00, improv_checksum(a, 2));
    uint8_t b[] = {0x04, 0x00}; TEST_ASSERT_EQUAL_HEX8(0x04, improv_checksum(b, 2));
}

void test_parse_scan(void) {
    uint8_t pkt[] = {0x04, 0x00, 0x04};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_OK, improv_parse_command(pkt, sizeof pkt, &c));
    TEST_ASSERT_EQUAL_UINT8(IMPROV_CMD_SCAN_WIFI, c.cmd);
}

void test_parse_send_wifi(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "hi", "pw");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_OK, improv_parse_command(pkt, n, &c));
    TEST_ASSERT_EQUAL_UINT8(IMPROV_CMD_SEND_WIFI, c.cmd);
    TEST_ASSERT_EQUAL_STRING("hi", c.ssid);
    TEST_ASSERT_EQUAL_STRING("pw", c.pass);
}

void test_open_network_empty_pass(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "hi", "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_OK, improv_parse_command(pkt, n, &c));
    TEST_ASSERT_EQUAL_STRING("", c.pass);
}

void test_reject_flag_smuggling_ssid(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "-x", "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, n, &c));
}

void test_reject_control_chars(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "ne\nt", "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, n, &c));
}

void test_reject_oversized_ssid(void) {
    char big[34]; memset(big, 'a', 33); big[33] = 0;
    uint8_t pkt[80]; size_t n; make_wifi(pkt, &n, big, "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, n, &c));
}

void test_reject_bad_checksum(void) {
    uint8_t pkt[] = {0x04, 0x00, 0xFF};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, sizeof pkt, &c));
}

void test_reject_length_mismatch(void) {
    uint8_t pkt[] = {0x01, 0x05, 0x00, 0x00};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, sizeof pkt, &c));
}

void test_unknown_command(void) {
    uint8_t pkt[] = {0x06, 0x00, 0x06};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_UNKNOWN_CMD, improv_parse_command(pkt, sizeof pkt, &c));
}

void test_encode_empty_scan_result(void) {
    uint8_t out[16];
    size_t n = improv_encode_result(IMPROV_CMD_SCAN_WIFI, NULL, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(3, n);
    uint8_t expect[] = {0x04, 0x00, 0x04};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, 3);
}

void test_encode_one_string(void) {
    const char *s[] = {"ok"};
    uint8_t out[16];
    size_t n = improv_encode_result(IMPROV_CMD_DEVICE_INFO, s, 1, out, sizeof out);
    uint8_t head[] = {0x03, 0x03, 0x02, 'o', 'k'};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, out, 5);
    TEST_ASSERT_EQUAL_HEX8(improv_checksum(out, n-1), out[n-1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_low_byte_sum);
    RUN_TEST(test_parse_scan);
    RUN_TEST(test_parse_send_wifi);
    RUN_TEST(test_open_network_empty_pass);
    RUN_TEST(test_reject_flag_smuggling_ssid);
    RUN_TEST(test_reject_control_chars);
    RUN_TEST(test_reject_oversized_ssid);
    RUN_TEST(test_reject_bad_checksum);
    RUN_TEST(test_reject_length_mismatch);
    RUN_TEST(test_unknown_command);
    RUN_TEST(test_encode_empty_scan_result);
    RUN_TEST(test_encode_one_string);
    return UNITY_END();
}
```

- [ ] **Step 4: Run tests to verify they fail to link**

Run: `pio test -e native -f test_improv`
Expected: FAIL — undefined references to `improv_checksum`, `improv_parse_command`, `improv_encode_result`.

- [ ] **Step 5: Write `src/improv.c`**

```c
#include "improv.h"
#include <string.h>

uint8_t improv_checksum(const uint8_t *b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
}

static int is_control(unsigned char c) { return c < 0x20 || c == 0x7f; }

static improv_parse_t parse_wifi(const uint8_t *d, size_t n, improv_command_t *out) {
    if (n < 1) return IMPROV_ERR_INVALID;
    size_t sl = d[0];
    if (1 + sl >= n) return IMPROV_ERR_INVALID;        // need pass_len byte after ssid
    size_t pl = d[1 + sl];
    if (1 + sl + 1 + pl != n) return IMPROV_ERR_INVALID;
    if (sl == 0 || sl > 32) return IMPROV_ERR_INVALID;
    if (pl > 64) return IMPROV_ERR_INVALID;
    memcpy(out->ssid, d + 1, sl); out->ssid[sl] = 0;
    memcpy(out->pass, d + 2 + sl, pl); out->pass[pl] = 0;
    if (out->ssid[0] == '-') return IMPROV_ERR_INVALID;             // nmcli flag-smuggle
    for (size_t i = 0; i < sl; i++) if (is_control((unsigned char)out->ssid[i])) return IMPROV_ERR_INVALID;
    for (size_t i = 0; i < pl; i++) if (is_control((unsigned char)out->pass[i])) return IMPROV_ERR_INVALID;
    out->cmd = IMPROV_CMD_SEND_WIFI;
    return IMPROV_OK;
}

improv_parse_t improv_parse_command(const uint8_t *pkt, size_t n, improv_command_t *out) {
    memset(out, 0, sizeof *out);
    if (n < 3) return IMPROV_ERR_INVALID;
    uint8_t cmd = pkt[0];
    size_t data_len = pkt[1];
    if (n != data_len + 3) return IMPROV_ERR_INVALID;
    if (improv_checksum(pkt, n - 1) != pkt[n - 1]) return IMPROV_ERR_INVALID;
    const uint8_t *data = pkt + 2;
    switch (cmd) {
        case IMPROV_CMD_SEND_WIFI: return parse_wifi(data, data_len, out);
        case IMPROV_CMD_SCAN_WIFI: out->cmd = cmd; return IMPROV_OK;
        case IMPROV_CMD_DEVICE_INFO: out->cmd = cmd; return IMPROV_OK;
        case IMPROV_CMD_IDENTIFY: out->cmd = cmd; return IMPROV_OK;
        default: return IMPROV_ERR_UNKNOWN_CMD;
    }
}

size_t improv_encode_result(uint8_t cmd, const char *const *strings, size_t count,
                            uint8_t *out, size_t cap) {
    if (cap < 3) return 0;
    size_t i = 0;
    out[i++] = cmd; out[i++] = 0;                       // length filled below
    for (size_t s = 0; s < count; s++) {
        size_t sl = strlen(strings[s]);
        if (i + 1 + sl + 1 > cap) return 0;
        out[i++] = (uint8_t)sl;
        memcpy(out + i, strings[s], sl); i += sl;
    }
    out[1] = (uint8_t)(i - 2);
    out[i] = improv_checksum(out, i); i++;
    return i;
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `pio test -e native -f test_improv`
Expected: PASS, 12 tests.

- [ ] **Step 7: Commit**

```bash
git add src/improv.h src/improv.c test/test_improv/test_improv.c platformio.ini
git commit -m "rover: pure Improv wire protocol (C port of hub improv.rs) + native tests"
```

---

### Task 2: Locator validation + robot-id formatting (pure)

**Files:**
- Create: `src/provisioning_util.h`, `src/provisioning_util.c`
- Create: `test/test_util/test_util.c`

**Interfaces:**
- Produces:
  - `bool rover_validate_locator(const char *s)` — true iff `tcp/<host>:<port>`, host non-empty no spaces/control, port 1–65535, total ≤ 64.
  - `void rover_format_robot_id(const uint8_t mac[6], char out[16])` — writes `rover-xxxx` (last 2 MAC bytes, lowercase hex).

- [ ] **Step 1: Write `src/provisioning_util.h`**

```c
#ifndef ROVER_PROVISIONING_UTIL_H
#define ROVER_PROVISIONING_UTIL_H
#include <stdbool.h>
#include <stdint.h>
bool rover_validate_locator(const char *s);
void rover_format_robot_id(const uint8_t mac[6], char out[16]);
#endif
```

- [ ] **Step 2: Write the failing tests `test/test_util/test_util.c`**

```c
#include <unity.h>
#include "provisioning_util.h"

void setUp(void) {}
void tearDown(void) {}

void test_valid_locator(void) {
    TEST_ASSERT_TRUE(rover_validate_locator("tcp/192.168.1.42:7447"));
    TEST_ASSERT_TRUE(rover_validate_locator("tcp/hub.local:7447"));
}
void test_reject_no_scheme(void)   { TEST_ASSERT_FALSE(rover_validate_locator("192.168.1.42:7447")); }
void test_reject_no_port(void)     { TEST_ASSERT_FALSE(rover_validate_locator("tcp/192.168.1.42")); }
void test_reject_port_zero(void)   { TEST_ASSERT_FALSE(rover_validate_locator("tcp/host:0")); }
void test_reject_port_huge(void)   { TEST_ASSERT_FALSE(rover_validate_locator("tcp/host:70000")); }
void test_reject_space(void)       { TEST_ASSERT_FALSE(rover_validate_locator("tcp/ho st:7447")); }
void test_reject_empty_host(void)  { TEST_ASSERT_FALSE(rover_validate_locator("tcp/:7447")); }
void test_reject_overlong(void) {
    char big[80]; for (int i=0;i<79;i++) big[i]='a'; big[79]=0;
    TEST_ASSERT_FALSE(rover_validate_locator(big));
}
void test_robot_id_format(void) {
    uint8_t mac[6] = {0x24,0x6f,0x28,0xaa,0x0d,0x08};
    char id[16]; rover_format_robot_id(mac, id);
    TEST_ASSERT_EQUAL_STRING("rover-0d08", id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_locator);
    RUN_TEST(test_reject_no_scheme);
    RUN_TEST(test_reject_no_port);
    RUN_TEST(test_reject_port_zero);
    RUN_TEST(test_reject_port_huge);
    RUN_TEST(test_reject_space);
    RUN_TEST(test_reject_empty_host);
    RUN_TEST(test_reject_overlong);
    RUN_TEST(test_robot_id_format);
    return UNITY_END();
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `pio test -e native -f test_util`
Expected: FAIL — undefined references.

- [ ] **Step 4: Write `src/provisioning_util.c`**

```c
#include "provisioning_util.h"
#include <stdio.h>
#include <string.h>

bool rover_validate_locator(const char *s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n == 0 || n > 64) return false;
    if (strncmp(s, "tcp/", 4) != 0) return false;
    const char *host = s + 4;
    const char *colon = strrchr(host, ':');
    if (!colon || colon == host) return false;           // need host and a port sep
    for (const char *p = host; p < colon; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return false;         // no space/control
    }
    const char *port = colon + 1;
    if (*port == 0) return false;
    long v = 0;
    for (const char *p = port; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        if (v > 65535) return false;
    }
    return v >= 1 && v <= 65535;
}

void rover_format_robot_id(const uint8_t mac[6], char out[16]) {
    snprintf(out, 16, "rover-%02x%02x", mac[4], mac[5]);
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e native -f test_util`
Expected: PASS, 9 tests.

- [ ] **Step 6: Commit**

```bash
git add src/provisioning_util.h src/provisioning_util.c test/test_util/test_util.c
git commit -m "rover: pure locator validation + MAC-derived robot-id, native tests"
```

---

### Task 3: NimBLE bring-up + flash-size de-risk (the early gate)

This task adds the BLE stack and proves the firmware still fits 4 MB **before** any feature work — the spec's primary risk, checked first.

**Files:**
- Modify: `src/CMakeLists.txt` (add sources so far + `REQUIRES bt`)
- Create: `sdkconfig.defaults` additions (NimBLE host, BT enabled)
- Modify: `src/main.c` (temporary: bring up NimBLE, advertise `rover-XXXX`, log, park)

**Interfaces:**
- Consumes: `rover_format_robot_id` (Task 2).
- Produces: a booting firmware advertising the rover name over BLE (temporary `main.c`, replaced in Task 7).

- [ ] **Step 1: Enable NimBLE in `sdkconfig.defaults`**

Append:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
```

- [ ] **Step 2: Update `src/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "main.c" "improv.c" "provisioning_util.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer bt)
```

- [ ] **Step 3: Temporary NimBLE advertiser in `src/main.c`**

Replace `app_main` with a minimal NimBLE bring-up (the rest of `main.c` from v1 is removed in Task 7; for now keep only what compiles). Full temporary file:

```c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "provisioning_util.h"

static const char *TAG = "rover-ble";
static char s_name[16];
static uint8_t s_addr_type;

static void advertise(void) {
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    struct ble_hs_adv_fields f = {0};
    f.name = (uint8_t *)s_name; f.name_len = strlen(s_name); f.name_is_complete = 1;
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_gap_adv_set_fields(&f);
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void host_task(void *param) { nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    rover_format_robot_id(mac, s_name);
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(s_name);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
```

- [ ] **Step 4: Build and check size fits**

Run: `pio run -e esp32dev`
Expected: build SUCCEEDS; in the output, the app partition usage line (`Flash: [ ... ] xx.x%`) is **< 100%** of the single-app-large partition. Record the percentage in the commit message.

- [ ] **Step 5: Flash and verify advertising on hardware**

Run: `pio run -e esp32dev -t upload && pio device monitor`
Expected: serial logs `advertising as rover-XXXX`. Confirm with a phone BLE scanner (e.g. nRF Connect) that `rover-XXXX` appears.

- [ ] **Step 6: Commit**

```bash
git add sdkconfig.defaults src/CMakeLists.txt src/main.c
git commit -m "rover: NimBLE bring-up advertising rover-XXXX; flash fits at <RECORD>%"
```

---

### Task 4: NVS config store

**Files:**
- Create: `src/rover_config.h`, `src/rover_config.c`
- Modify: `src/CMakeLists.txt` (add `rover_config.c`)

**Interfaces:**
- Consumes: `rover_validate_locator` (Task 2).
- Produces:
  - `bool rover_config_load(char ssid[33], char pass[65], char locator[65])` — true iff all present.
  - `esp_err_t rover_config_set_wifi(const char *ssid, const char *pass)`
  - `esp_err_t rover_config_set_locator(const char *locator)`
  - `bool rover_config_is_complete(void)` — ssid present AND locator present.
  - `void rover_config_clear(void)`

NVS namespace `"rover"`, keys `"ssid"`, `"pass"`, `"locator"`.

- [ ] **Step 1: Write `src/rover_config.h`**

```c
#ifndef ROVER_CONFIG_H
#define ROVER_CONFIG_H
#include <stdbool.h>
#include "esp_err.h"
bool rover_config_load(char ssid[33], char pass[65], char locator[65]);
esp_err_t rover_config_set_wifi(const char *ssid, const char *pass);
esp_err_t rover_config_set_locator(const char *locator);
bool rover_config_is_complete(void);
void rover_config_clear(void);
#endif
```

- [ ] **Step 2: Write `src/rover_config.c`**

```c
#include "rover_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "provisioning_util.h"

#define NS "rover"

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    return nvs_get_str(h, key, out, &len) == ESP_OK && len > 0;
}

bool rover_config_load(char ssid[33], char pass[65], char locator[65]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = get_str(h, "ssid", ssid, 33) && get_str(h, "locator", locator, 65);
    size_t pl = 65;
    if (nvs_get_str(h, "pass", pass, &pl) != ESP_OK) pass[0] = 0;   // empty = open net
    nvs_close(h);
    return ok && rover_validate_locator(locator);
}

esp_err_t rover_config_set_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass ? pass : "");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t rover_config_set_locator(const char *locator) {
    if (!rover_validate_locator(locator)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "locator", locator);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool rover_config_is_complete(void) {
    char s[33], p[65], l[65];
    return rover_config_load(s, p, l);
}

void rover_config_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
}
```

- [ ] **Step 3: Add to build `src/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "main.c" "improv.c" "provisioning_util.c" "rover_config.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer bt)
```

- [ ] **Step 4: Verify on hardware via a temporary self-test**

Temporarily add to the end of `app_main` (after `rover_format_robot_id`):

```c
rover_config_set_locator("tcp/10.0.0.5:7447");
ESP_LOGI(TAG, "is_complete after locator-only = %d (expect 0, no ssid)", rover_config_is_complete());
rover_config_set_wifi("TestNet", "pw");
ESP_LOGI(TAG, "is_complete after both = %d (expect 1)", rover_config_is_complete());
rover_config_clear();
ESP_LOGI(TAG, "is_complete after clear = %d (expect 0)", rover_config_is_complete());
```

Run: `pio run -e esp32dev -t upload && pio device monitor`
Expected: logs show `0`, then `1`, then `0`. Then **remove the temporary block**.

- [ ] **Step 5: Commit**

```bash
git add src/rover_config.h src/rover_config.c src/CMakeLists.txt
git commit -m "rover: NVS config store (ssid/pass/locator) with completeness predicate"
```

---

### Task 5: `hubcfg` GATT service (locator characteristic)

**Files:**
- Create: `src/provisioning.h`, `src/provisioning.c`
- Modify: `src/CMakeLists.txt` (add `provisioning.c`)

**Interfaces:**
- Consumes: `rover_config_set_locator` (Task 4), `rover_validate_locator` (Task 2).
- Produces:
  - `#define HUBCFG_SERVICE_UUID128` — **finalize here**: generate a random 128-bit UUID (`python3 -c "import uuid;print(uuid.uuid4())"`), record it as the canonical constant. Record the same value in `rover.html` (Task 8).
  - `void provisioning_register_hubcfg(void)` — registers the GATT service.
  - characteristic write handler validates via `rover_validate_locator`, persists via `rover_config_set_locator`; read returns the stored locator.

- [ ] **Step 1: Generate and record the UUID**

Run: `python3 -c "import uuid; print(uuid.uuid4())"`
Record the output (e.g. `b3e6...`) as `HUBCFG_SERVICE_UUID` in `src/provisioning.h` and note it for Task 8. The `locator` characteristic UUID = same base with the final group `...01`.

- [ ] **Step 2: Write `src/provisioning.h`**

```c
#ifndef ROVER_PROVISIONING_H
#define ROVER_PROVISIONING_H
// Canonical hubcfg UUIDs — MUST match better-robotics/provision/rover.html.
#define HUBCFG_SERVICE_UUID "PASTE-GENERATED-UUID-HERE"
#define HUBCFG_LOCATOR_UUID "PASTE-GENERATED-UUID-HERE-with-final-...01"
void provisioning_register_hubcfg(void);
#endif
```

- [ ] **Step 3: Write `src/provisioning.c` (hubcfg service)**

```c
#include "provisioning.h"
#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "rover_config.h"
#include "provisioning_util.h"

static const char *TAG = "hubcfg";
static ble_uuid128_t s_svc_uuid, s_loc_uuid;
static char s_locator[65];

static int loc_access(uint16_t ch, uint16_t a, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, s_locator, strlen(s_locator)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[65] = {0};
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n >= sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf - 1, &n);
        if (!rover_validate_locator(buf)) { ESP_LOGW(TAG, "rejected locator '%s'", buf); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; }
        if (rover_config_set_locator(buf) != ESP_OK) return BLE_ATT_ERR_UNLIKELY;
        strncpy(s_locator, buf, sizeof s_locator - 1);
        ESP_LOGI(TAG, "locator set: %s", s_locator);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void provisioning_register_hubcfg(void) {
    ble_uuid_init_from_buf((ble_uuid_any_t *)&s_svc_uuid, NULL, 0); // placeholder; set below
    // Parse the string UUIDs into 128-bit structs.
    ble_uuid_from_str((ble_uuid_any_t *)&s_svc_uuid, HUBCFG_SERVICE_UUID);
    ble_uuid_from_str((ble_uuid_any_t *)&s_loc_uuid, HUBCFG_LOCATOR_UUID);

    static struct ble_gatt_chr_def chrs[2];
    chrs[0] = (struct ble_gatt_chr_def){
        .uuid = &s_loc_uuid.u,
        .access_cb = loc_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    };
    chrs[1] = (struct ble_gatt_chr_def){0};
    static struct ble_gatt_svc_def svcs[2];
    svcs[0] = (struct ble_gatt_svc_def){
        .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_svc_uuid.u, .characteristics = chrs,
    };
    svcs[1] = (struct ble_gatt_svc_def){0};
    ble_gatts_count_cfg(svcs);
    ble_gatts_add_svcs(svcs);
}
```

> Note: `ble_uuid_from_str` is a small helper — if the installed NimBLE lacks it, parse the hyphenated string into the 16 bytes of `ble_uuid128_t.value` (little-endian) in a static initializer instead. The first build will tell you; adjust to a byte array literal if needed.

- [ ] **Step 4: Register it from `app_main` (temporary wiring) and add to build**

In `src/CMakeLists.txt` add `provisioning.c`. In `app_main`, after `ble_svc_gap_init();` add `provisioning_register_hubcfg();` (before advertising).

- [ ] **Step 5: Verify on hardware**

Run: `pio run -e esp32dev -t upload && pio device monitor`
From nRF Connect: connect to `rover-XXXX`, find the hubcfg service, write `tcp/10.0.0.5:7447` to the locator characteristic → serial logs `locator set: tcp/10.0.0.5:7447`; read back returns the same. Write `garbage` → serial logs `rejected locator 'garbage'` and the write errors.

- [ ] **Step 6: Commit**

```bash
git add src/provisioning.h src/provisioning.c src/CMakeLists.txt
git commit -m "rover: hubcfg GATT service — validated locator characteristic to NVS"
```

---

### Task 6: Improv GATT service (Wi-Fi over stock Improv)

**Files:**
- Modify: `src/provisioning.h`, `src/provisioning.c` (add Improv service + Wi-Fi join)

**Interfaces:**
- Consumes: `improv_parse_command`, `improv_encode_result` (Task 1), `rover_config_set_wifi` (Task 4).
- Produces:
  - `void provisioning_register_improv(void)` — registers the 5 Improv characteristics.
  - `typedef void (*provisioning_done_cb)(void);` set via `void provisioning_set_done_cb(provisioning_done_cb)` — invoked when both Wi-Fi creds and locator are present.
  - On `SEND_WIFI`: validates the credentials against the live AP via `esp_wifi`, persists via `rover_config_set_wifi`, notifies state `PROVISIONING`→`PROVISIONED` (or error), and if `rover_config_is_complete()` calls the done cb.

- [ ] **Step 1: Add Improv state to `provisioning.c`**

Add notify handles + the State/Error/RPC-Result/Capabilities/RPC-Command characteristics, backed by static current-state bytes. Wire the RPC-Command write handler to `improv_parse_command`; on `SEND_WIFI`, attempt a Wi-Fi connect (STA, scan+join), and:

```c
// pseudocode-level, real NimBLE calls:
// state = PROVISIONING; notify(state_chr)
// if wifi_try_join(ssid, pass): rover_config_set_wifi(ssid,pass);
//    state = PROVISIONED; notify; result = encode_result(SEND_WIFI, NULL, 0); notify(rpc_res)
//    if rover_config_is_complete() done_cb();
// else: error = UNABLE_CONNECT; notify(error_chr); state = AUTHORIZED; notify
```

Capabilities characteristic returns `{IMPROV_CAP_SCAN_WIFI}`. State characteristic initial value `IMPROV_STATE_AUTHORIZED`. (The full handler mirrors `hub/src/bin/provisiond.rs` `handle_command`/`do_join`; reuse its structure.)

> **Wi-Fi-in-provisioning caveat (Global Constraint):** joining Wi-Fi to validate creds means the radio briefly does Wi-Fi while BLE is up. To honor "never both at once", do NOT validate by joining here — **persist the creds and report PROVISIONED optimistically**, then let operating mode (Task 7) do the real join. If the join later fails N times, operating mode falls back to provisioning. This keeps BLE and Wi-Fi temporally separate. Implement the optimistic path.

- [ ] **Step 2: Implement the Improv characteristics (real NimBLE)**

Add to `provisioning.c` an Improv service def alongside hubcfg, with 5 characteristics using the UUIDs from `improv.h`. State/Error are READ+NOTIFY (1 byte), RPC-Result is NOTIFY, RPC-Command is WRITE, Capabilities is READ. Store `ble_gatts_find_chr` value handles for notify. The RPC-Command write handler:

```c
static int rpc_cmd_access(uint16_t ch, uint16_t a, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[140]; uint16_t n = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n);
    improv_command_t c;
    improv_parse_t r = improv_parse_command(buf, n, &c);
    if (r == IMPROV_OK && c.cmd == IMPROV_CMD_SEND_WIFI) {
        set_state(IMPROV_STATE_PROVISIONING);
        rover_config_set_wifi(c.ssid, c.pass);          // optimistic persist (see caveat)
        set_state(IMPROV_STATE_PROVISIONED);
        uint8_t out[8]; size_t on = improv_encode_result(IMPROV_CMD_SEND_WIFI, NULL, 0, out, sizeof out);
        notify_rpc_result(out, on);
        if (rover_config_is_complete() && s_done_cb) s_done_cb();
    } else if (r == IMPROV_OK && c.cmd == IMPROV_CMD_DEVICE_INFO) {
        const char *info[] = {"rover", "2", "ESP32", ""};   // name filled at runtime
        uint8_t out[64]; size_t on = improv_encode_result(IMPROV_CMD_DEVICE_INFO, info, 4, out, sizeof out);
        notify_rpc_result(out, on);
    } else if (r != IMPROV_OK) {
        set_error(r == IMPROV_ERR_UNKNOWN_CMD ? IMPROV_ERR_UNKNOWN_RPC : IMPROV_ERR_INVALID_PKT);
    }
    return 0;
}
```

Provide `set_state`, `set_error`, `notify_rpc_result` helpers that update the stored byte and `ble_gatts_notify_custom` on the right handle. Advertise the Improv **service UUID** in the advertising data (so the stock client filters it in) and the name in the scan response — mirror `hub/src/bin/provisiond.rs start_legacy_advert`'s data layout (service UUID in adv, name in scan-rsp).

- [ ] **Step 3: Verify on hardware with the real Improv client**

Run: `pio run -e esp32dev -t upload && pio device monitor`
From desktop Chrome on `better-robotics/provision` (hub page works for any Improv device) OR improv-wifi.com: connect to `rover-XXXX`, send Wi-Fi creds. Expected serial: state→PROVISIONING→PROVISIONED, creds logged/persisted. `rover_config_is_complete` still false until a locator is also set.

- [ ] **Step 4: Commit**

```bash
git add src/provisioning.h src/provisioning.c
git commit -m "rover: Improv GATT service — Wi-Fi creds to NVS (optimistic, validated in operating mode)"
```

---

### Task 7: Mode dispatch + operating mode + re-provisioning

**Files:**
- Modify: `src/main.c` (replace temporary bring-up with real mode dispatch + the v1 operating logic, NVS-backed)

**Interfaces:**
- Consumes: `rover_config_is_complete`, `rover_config_load`, `rover_config_clear` (Task 4); `provisioning_register_improv`/`_hubcfg`, `provisioning_set_done_cb` (Tasks 5–6); `rover_format_robot_id` (Task 2).
- Produces: the shipping firmware. Provisioning mode (BLE) when `!is_complete`; operating mode (Wi-Fi STA from NVS → zenoh client → publish `robots/rover-XXXX/sys`) when complete; on N consecutive Wi-Fi/`z_open` failures, `rover_config_clear()`-of-nothing → `esp_restart()` into provisioning. **Do not clear creds on failure** — only re-enter provisioning mode so the operator can overwrite; clearing would lose a possibly-correct SSID on a transient outage. Re-provisioning = restart into BLE mode while keeping NVS; the new writes overwrite.

> **Re-provisioning refinement:** "fall back to provisioning" must not wipe good creds on a transient hub reboot. Instead: count failures; after N=5, set BLE mode for the *next* boot via a RAM-retained flag is unreliable across deep resets — so persist a small `"bootmode"` NVS byte: operating mode sets it to `OPERATING`; on N failures set it to `PROVISION` and restart; provisioning mode, once it accepts a new write, sets it back to `OPERATING`. Boot dispatch reads `is_complete() && bootmode != PROVISION`.

- [ ] **Step 1: Write the final `src/main.c`**

```c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "zenoh-pico.h"
#include "rover_config.h"
#include "provisioning_util.h"
#include "provisioning.h"

static const char *TAG = "rover";
static char s_id[16];
static volatile bool s_got_ip = false;

// ---- operating mode: wifi STA + zenoh client (v1 logic, NVS-backed) ----
static void on_evt(void *a, esp_event_base_t base, int32_t id, void *d) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { s_got_ip = false; esp_wifi_connect(); }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) s_got_ip = true;
}

static bool wifi_start(const char *ssid, const char *pass) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL);
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    for (int i = 0; i < 40 && !s_got_ip; i++) vTaskDelay(pdMS_TO_TICKS(250)); // ~10s
    return s_got_ip;
}

static void operating_mode(const char *ssid, const char *pass, const char *locator) {
    static int fails = 0;
    if (!wifi_start(ssid, pass)) { goto fail; }
    z_owned_config_t config; z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client");
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, locator);
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) { ESP_LOGE(TAG, "z_open failed -> %s", locator); goto fail; }
    z_owned_publisher_t pub; z_view_keyexpr_t ke;
    char key[40]; snprintf(key, sizeof key, "robots/%s/sys", s_id);
    z_view_keyexpr_from_str(&ke, key);
    if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0) goto fail;
    rover_config_set_bootmode_operating();   // success: clear any re-provision flag
    ESP_LOGI(TAG, "publishing %s every 2s", key);
    char buf[160];
    for (;;) {
        int64_t up = esp_timer_get_time()/1000; uint32_t heap = esp_get_free_heap_size();
        snprintf(buf, sizeof buf, "{\"uptime_ms\":%lld,\"free_heap\":%u,\"synthetic\":false}", (long long)up, (unsigned)heap);
        z_owned_bytes_t payload; z_bytes_copy_from_str(&payload, buf);
        z_publisher_put(z_loan(pub), z_move(payload), NULL);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
fail:
    if (++fails >= 5) { rover_config_set_bootmode_provision(); }
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

// ---- provisioning mode: BLE (Improv + hubcfg) ----
static void on_provision_done(void) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
extern void provisioning_advertise(const char *name); // defined in provisioning.c

static void on_sync(void) { provisioning_advertise(s_id); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

static void provisioning_mode(void) {
    ESP_LOGI(TAG, "provisioning mode — advertising %s", s_id);
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(s_id);
    provisioning_register_hubcfg();
    provisioning_register_improv();
    provisioning_set_done_cb(on_provision_done);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    rover_format_robot_id(mac, s_id);
    char ssid[33], pass[65], loc[65];
    if (rover_config_is_complete() && !rover_config_bootmode_is_provision() && rover_config_load(ssid, pass, loc))
        operating_mode(ssid, pass, loc);
    else
        provisioning_mode();
}
```

- [ ] **Step 2: Add the bootmode helpers to `rover_config.c`/`.h`**

```c
// rover_config.h
void rover_config_set_bootmode_operating(void);
void rover_config_set_bootmode_provision(void);
bool rover_config_bootmode_is_provision(void);
```

```c
// rover_config.c
static void set_bootmode(uint8_t v){ nvs_handle_t h; if(nvs_open(NS,NVS_READWRITE,&h)==ESP_OK){ nvs_set_u8(h,"bootmode",v); nvs_commit(h); nvs_close(h);} }
void rover_config_set_bootmode_operating(void){ set_bootmode(0); }
void rover_config_set_bootmode_provision(void){ set_bootmode(1); }
bool rover_config_bootmode_is_provision(void){ nvs_handle_t h; uint8_t v=0; if(nvs_open(NS,NVS_READONLY,&h)==ESP_OK){ nvs_get_u8(h,"bootmode",&v); nvs_close(h);} return v==1; }
```

Also: when `provisioning.c` accepts any successful write (locator or wifi), call `rover_config_set_bootmode_operating()` so a re-provisioned rover boots into operating mode next.

- [ ] **Step 3: Add `provisioning_advertise` to `provisioning.c`**

A function that sets adv fields (Improv service UUID in adv data, name in scan response) and starts advertising — factor from Task 6 Step 2.

- [ ] **Step 4: Full end-to-end hardware verification**

Run: `pio run -e esp32dev -t upload && pio device monitor`
1. Fresh flash (NVS empty) → boots **provisioning mode**, advertises `rover-XXXX`.
2. From `rover.html` (Task 8) or nRF Connect + Chrome Improv: set Wi-Fi, then write locator → rover reboots → **operating mode** → joins Wi-Fi → `z_open` → publishes `robots/rover-XXXX/sys`. Confirm on the hub (`watch` subscriber) the messages arrive.
3. Power-cycle → rejoins from NVS, **no BLE** (operating mode straight away).
4. Set locator to a dead `tcp/<bad-ip>:7447` and reboot → after 5 fails, falls back to provisioning mode (advertises again).

- [ ] **Step 5: Commit**

```bash
git add src/main.c src/rover_config.c src/rover_config.h src/provisioning.c src/provisioning.h
git commit -m "rover: mode dispatch + NVS-backed operating mode + re-provisioning fallback"
```

- [ ] **Step 6: Update rover docs**

Update `README.md` and `CLAUDE.md`: replace "hardcodes SSID + locator… set these before flashing" with the BLE-provisioning flow; note `rover-XXXX` identity; mark the roadmap item done. Commit:

```bash
git add README.md CLAUDE.md
git commit -m "rover: document BLE provisioning (replaces hardcoded config)"
```

---

### Task 8: Provision page — `rover.html`

**Files:**
- Create: `better-robotics/provision/rover.html`

**Interfaces:**
- Consumes: the `hubcfg` service + `locator` characteristic UUIDs from Task 5 (must match exactly).

- [ ] **Step 1: Write `rover.html`**

Reuse the vendored Improv SDK for Wi-Fi; add a small Web-Bluetooth control for the locator. Card layout matching `index.html`.

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" /><meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Better Robotics — Rover setup</title>
  <link rel="stylesheet" href="data:text/css,"> <!-- reuse index.html styles inline below -->
  <style>/* copy the .card / button styling from index.html for consistency */</style>
</head>
<body>
  <main class="card">
    <span class="badge">Better Robotics</span>
    <h1>Rover setup</h1>
    <p class="sub">Two steps: join Wi-Fi, then point the rover at your hub.</p>

    <h2>1 · Wi-Fi</h2>
    <improv-wifi-launch-button>
      <button slot="activate">Set rover Wi-Fi</button>
      <span slot="unsupported">Use desktop Chrome or Edge.</span>
    </improv-wifi-launch-button>

    <h2>2 · Hub address</h2>
    <input id="loc" placeholder="tcp/192.168.1.42:7447" />
    <button id="setloc">Set hub address</button>
    <p id="status" class="note"></p>
  </main>

  <script type="module" src="vendor/improv-ble/launch-button.js"></script>
  <script>
    const SVC = "PASTE-HUBCFG-SERVICE-UUID";   // must match src/provisioning.h
    const CHR = "PASTE-HUBCFG-LOCATOR-UUID";
    const $ = (id) => document.getElementById(id);
    function valid(s){ return /^tcp\/[^\s:]+:\d{1,5}$/.test(s) && (+s.split(":").pop() <= 65535); }
    $("setloc").onclick = async () => {
      const v = $("loc").value.trim();
      if (!valid(v)) { $("status").textContent = "Enter tcp/<ip>:<port>"; return; }
      try {
        const dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [SVC] }] });
        const gatt = await dev.gatt.connect();
        const svc = await gatt.getPrimaryService(SVC);
        const chr = await svc.getCharacteristic(CHR);
        await chr.writeValue(new TextEncoder().encode(v));
        const back = new TextDecoder().decode(await chr.readValue());
        $("status").textContent = back === v ? `Hub set: ${back}` : `Wrote, device reports: ${back}`;
        gatt.disconnect();
      } catch (e) { $("status").textContent = "Failed: " + e.message; }
    };
  </script>
</body>
</html>
```

- [ ] **Step 2: Paste the real UUIDs**

Replace `SVC`/`CHR` with the exact values from `src/provisioning.h` (Task 5). Copy the `.card`/button CSS from `index.html` into the `<style>` block.

- [ ] **Step 3: Verify in the browser (served over HTTPS)**

Push to `better-robotics/provision`, wait for Pages, open `https://better-robotics.github.io/provision/rover.html` in desktop Chrome. With a provisioning-mode rover powered on: step 1 sets Wi-Fi (Improv), step 2 writes the locator (status shows `Hub set: tcp/...`). Confirm the rover reboots and publishes.

- [ ] **Step 4: Commit (in the provision repo)**

```bash
cd ../provision
git add rover.html
git commit -m "Add rover setup page: Improv Wi-Fi + hub locator over BLE"
git push
```

---

## Self-Review

**Spec coverage:**
- Provision Wi-Fi over Improv → Tasks 1, 6, 8. ✓
- Provision locator over custom GATT char → Tasks 5, 8. ✓
- Reject multicast scouting; locator provisioned → design honored (no scout code). ✓
- Mode-switched NimBLE, never both radios → Tasks 3, 6 (caveat: optimistic persist, no join-in-provisioning), 7. ✓
- NVS schema (ssid/pass/locator) + completeness → Task 4. ✓
- Re-provisioning on N failures → Task 7 (bootmode flag, N=5, no cred-wipe). ✓
- Identity rover-XXXX from MAC → Tasks 2, 3, 7. ✓
- Remove hardcodes → Task 7. ✓
- hubcfg contract (UUID + format + validation) → Tasks 2, 5. ✓
- Flash/RAM risk verified early → Task 3 (records %). ✓
- Testing: host unit (Tasks 1–2) + on-hardware checklist (Tasks 3–8). ✓
- Page sibling, hub page untouched → Task 8. ✓

**Placeholder scan:** UUID placeholders in Tasks 5/8 are *intentional generate-and-record* steps with explicit instructions, not vague TODOs. The Task 6 Step 1 pseudocode is followed by Step 2's real NimBLE handler — no code-free implementation step ships. The `ble_uuid_from_str` note gives a concrete fallback. No "add error handling"/"handle edge cases" left abstract.

**Type consistency:** `rover_config_*`, `improv_*`, `provisioning_*`, `rover_validate_locator`, `rover_format_robot_id` names are consistent across producer/consumer blocks. `bootmode` helpers defined (Task 7 Step 2) before use (Task 7 Step 1). `provisioning_advertise` declared `extern` in main and defined in Task 7 Step 3.

**Known soft spots flagged for the implementer (not blockers):** NimBLE GATT registration API surface (`ble_gatts_add_svcs` vs `ble_gatts_count_cfg`+`ble_gatts_register_svcs`) varies slightly by ESP-IDF version — the first build resolves it; Task 5's note covers the UUID-parse variant.
