#include "provisioning.h"
#include "improv.h"
#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "rover_config.h"
#include "provisioning_util.h"

static const char *TAG = "hubcfg";

static provisioning_done_cb s_done_cb;

/*
 * UUIDs as 128-bit little-endian byte literals (NimBLE ble_uuid128_t format).
 *
 * Service: dd001f17-5e75-4dfc-b611-70ef1e6bb9ca
 * Char:    4941adfa-0a40-460f-9096-39d1db36f53b
 *
 * To convert: take the UUID bytes in standard order, then reverse them.
 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xca, 0xb9, 0x6b, 0x1e, 0xef, 0x70, 0x11, 0xb6,
    0xfc, 0x4d, 0x75, 0x5e, 0x17, 0x1f, 0x00, 0xdd
);

static const ble_uuid128_t s_loc_uuid = BLE_UUID128_INIT(
    0x3b, 0xf5, 0x36, 0xdb, 0xd1, 0x39, 0x96, 0x90,
    0x0f, 0x46, 0x40, 0x0a, 0xfa, 0xad, 0x41, 0x49
);

static char s_locator[65];

static int loc_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* Load current value from NVS on each read for freshness */
        char ssid[33], pass[65], loc[65];
        const char *val = s_locator;
        if (s_locator[0] == '\0') {
            /* Try loading from NVS if in-memory cache is empty */
            if (rover_config_load(ssid, pass, loc)) {
                strncpy(s_locator, loc, sizeof(s_locator) - 1);
                val = s_locator;
            }
        }
        return os_mbuf_append(ctxt->om, val, strlen(val)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[65] = {0};
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n >= sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &n);
        buf[n] = '\0';

        if (!rover_validate_locator(buf)) {
            ESP_LOGW(TAG, "rejected locator '%s'", buf);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (rover_config_set_locator(buf) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        strncpy(s_locator, buf, sizeof(s_locator) - 1);
        ESP_LOGI(TAG, "locator set: %s", s_locator);
        /* Order-independent completion: if Wi-Fi was already written first,
         * this second write completes provisioning and arms the debounced reboot. */
        if (rover_config_is_complete() && s_done_cb) s_done_cb();
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── Improv GATT service ─────────────────────────────────────────────────── */

/*
 * UUID bytes in little-endian order (NimBLE's BLE_UUID128_INIT layout).
 *
 * String form:  00467768-6228-2272-4663-277478268000
 * Big-endian:   00 46 77 68  62 28  22 72  46 63  27 74 78 26 80 00
 * Little-endian (reversed): 00 80 26 78 74 27 63 46 72 22 28 62 68 77 46 00
 *
 * Chars differ only in byte [0] (the "xxxx" low byte of the last field):
 *   State  ...8001 → 01 80 ...
 *   Error  ...8002 → 02 80 ...
 *   RpcCmd ...8003 → 03 80 ...
 *   RpcRes ...8004 → 04 80 ...
 *   Caps   ...8005 → 05 80 ...
 */
static const ble_uuid128_t provisioning_improv_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00
);
static const ble_uuid128_t s_improv_state_uuid = BLE_UUID128_INIT(
    0x01, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00
);
static const ble_uuid128_t s_improv_error_uuid = BLE_UUID128_INIT(
    0x02, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00
);
static const ble_uuid128_t s_improv_rpccmd_uuid = BLE_UUID128_INIT(
    0x03, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00
);
static const ble_uuid128_t s_improv_rpcres_uuid = BLE_UUID128_INIT(
    0x04, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00
);
static const ble_uuid128_t s_improv_caps_uuid = BLE_UUID128_INIT(
    0x05, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00
);

/* Live state bytes */
static uint8_t s_improv_state = IMPROV_STATE_AUTHORIZED;
static uint8_t s_improv_error = IMPROV_ERR_NONE;

/* Value handles for notify (populated by NimBLE after gatts_add_svcs) */
static uint16_t s_state_val_handle;
static uint16_t s_error_val_handle;
static uint16_t s_rpcres_val_handle;

/* Last connected handle, for async notify. NimBLE runs single-connection in
 * this firmware, so one slot is sufficient. */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

void provisioning_set_done_cb(provisioning_done_cb cb) { s_done_cb = cb; }

bool provisioning_client_connected(void) {
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

static void notify_u8(uint16_t val_handle, uint8_t v) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&v, 1);
    if (om) ble_gatts_notify_custom(s_conn_handle, val_handle, om);
}

static void set_state(uint8_t st) {
    s_improv_state = st;
    notify_u8(s_state_val_handle, st);
}

static void set_error(uint8_t err) {
    s_improv_error = err;
    notify_u8(s_error_val_handle, err);
}

static void notify_rpc_result(const uint8_t *data, size_t len) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) ble_gatts_notify_custom(s_conn_handle, s_rpcres_val_handle, om);
}

/* GATT access callbacks */

static int improv_state_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, &s_improv_state, 1) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int improv_error_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, &s_improv_error, 1) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int improv_caps_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    /* No optional caps: SCAN_WIFI is unimplemented (no rpccmd branch), so
     * advertising it would hang a scanning client waiting for a reply that
     * never comes. Client falls back to manual SSID/password entry. */
    uint8_t caps = 0;
    return os_mbuf_append(ctxt->om, &caps, 1) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int improv_rpcres_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    /* Notify-only; no read value exposed */
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;
}

static int improv_rpccmd_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t buf[140]; uint16_t n = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n);

    improv_command_t c;
    improv_parse_t r = improv_parse_command(buf, n, &c);

    if (r == IMPROV_OK && c.cmd == IMPROV_CMD_SEND_WIFI) {
        set_state(IMPROV_STATE_PROVISIONING);
        /* Optimistic persist — radios stay temporally separate. Operating mode
         * does the real join; on failure it falls back to a provisioning window. */
        rover_config_set_wifi(c.ssid, c.pass);
        ESP_LOGI(TAG, "wifi creds persisted: %s", c.ssid);
        set_state(IMPROV_STATE_PROVISIONED);
        uint8_t out[8];
        size_t on = improv_encode_result(IMPROV_CMD_SEND_WIFI, NULL, 0, out, sizeof out);
        notify_rpc_result(out, on);
        if (rover_config_is_complete() && s_done_cb) s_done_cb();
    } else if (r == IMPROV_OK && c.cmd == IMPROV_CMD_DEVICE_INFO) {
        const char *info[] = {"rover", "2", "ESP32", ""};
        uint8_t out[64];
        size_t on = improv_encode_result(IMPROV_CMD_DEVICE_INFO, info, 4, out, sizeof out);
        notify_rpc_result(out, on);
    } else if (r != IMPROV_OK) {
        set_error(r == IMPROV_ERR_UNKNOWN_CMD ? IMPROV_ERR_UNKNOWN_RPC : IMPROV_ERR_INVALID_PKT);
    }
    return 0;
}

static const struct ble_gatt_chr_def s_improv_chrs[] = {
    {
        .uuid       = &s_improv_state_uuid.u,
        .access_cb  = improv_state_access,
        .val_handle = &s_state_val_handle,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid       = &s_improv_error_uuid.u,
        .access_cb  = improv_error_access,
        .val_handle = &s_error_val_handle,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid       = &s_improv_rpccmd_uuid.u,
        .access_cb  = improv_rpccmd_access,
        .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = &s_improv_rpcres_uuid.u,
        .access_cb  = improv_rpcres_access,
        .val_handle = &s_rpcres_val_handle,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid       = &s_improv_caps_uuid.u,
        .access_cb  = improv_caps_access,
        .flags      = BLE_GATT_CHR_F_READ,
    },
    { 0 }, /* terminator */
};

/* Combined table: hubcfg + Improv.  NimBLE is most reliable with a single
 * ble_gatts_add_svcs call; splitting across multiple calls risks the
 * attribute-count preallocation (ble_gatts_count_cfg) not accounting for
 * the later services and silently failing to register them. */
static const struct ble_gatt_chr_def s_hubcfg_chrs[] = {
    {
        .uuid       = &s_loc_uuid.u,
        .access_cb  = loc_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_all_svcs[] = {
    /* hubcfg */
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_svc_uuid.u,
        .characteristics = s_hubcfg_chrs,
    },
    /* Improv */
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &provisioning_improv_svc_uuid.u,
        .characteristics = s_improv_chrs,
    },
    { 0 }, /* terminator */
};

void provisioning_register(void) {
    int rc = ble_gatts_count_cfg(s_all_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return; }
    rc = ble_gatts_add_svcs(s_all_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return; }
    ESP_LOGI(TAG, "hubcfg + Improv GATT services registered");
}

/* ── Advertising ─────────────────────────────────────────────────────────── */

/* Cached name so the GAP event callback can re-advertise after disconnect. */
static char s_adv_name[16];
static uint8_t s_addr_type;

static int prov_gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            provisioning_advertise(s_adv_name);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        provisioning_advertise(s_adv_name);
        break;
    default:
        break;
    }
    return 0;
}

void provisioning_advertise(const char *name) {
    strncpy(s_adv_name, name, sizeof(s_adv_name) - 1);
    s_adv_name[sizeof(s_adv_name) - 1] = '\0';

    ble_hs_id_infer_auto(0, &s_addr_type);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* Adv packet: flags + Improv service UUID */
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &provisioning_improv_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    /* Scan-response: complete local name */
    struct ble_hs_adv_fields sr = {0};
    sr.name = (uint8_t *)s_adv_name;
    sr.name_len = strlen(s_adv_name);
    sr.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&sr);
    if (rc) { ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc); }

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, prov_gap_event_cb, NULL);
    if (rc) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as %s", s_adv_name);
}
