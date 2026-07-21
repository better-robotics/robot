/* POST /ota — push firmware over the classroom Wi-Fi. See ota_update.h. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/base64.h"

#include "ota_update.h"
#include "roles.h"
#include "wifi_portal.h"

static const char *TAG = "ota";

/* One TCP read's worth. 1 KB, not 4: this buffer is on the httpd task's stack,
 * and the image is streamed through it — never buffered whole, since 1.3 MB is
 * far past any heap this chip has. */
#define OTA_CHUNK 1024

/* Grace before the running image is declared good — see the mark task below. */
#define OTA_SELFTEST_MS 30000

/* ── auth ─────────────────────────────────────────────────────────────────────
 * HTTP Basic as "operator", against the same secret the broker's session auth
 * uses (board_operator_pass_ok, hub_role.c).
 *
 * Basic sends base64, which is not encryption — on this plain-HTTP :80 the
 * password is effectively on the wire in clear. That is deliberate and matches
 * the rest of the firmware: the hub's own Wi-Fi is the real perimeter
 * (CONTRACT.md § Discovery & isolation), and this gate exists for the same
 * reason fleet/estop's does — deliberate friction on an action that can take
 * the whole room down, not a claim of secrecy against someone already on the
 * network. TLS here would need a CA-signed cert for an mDNS name, which a board
 * cannot have.
 *
 * It IS a header and not a query parameter on purpose: a password in a URL
 * lands in logs and browser history. */
static bool basic_auth_ok(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 200) return false;

    char hdr[201];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    unsigned char dec[160];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof dec - 1, &dlen,
                              (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0)
        return false;
    dec[dlen] = 0;

    /* "user:pass" — split at the FIRST colon; a password may contain colons. */
    char *colon = strchr((char *)dec, ':');
    if (!colon) return false;
    *colon = 0;
    const char *user = (const char *)dec, *pass = colon + 1;

    if (strcmp(user, "operator") != 0) return false;
    return board_operator_pass_ok(pass);
}

/* ── mark-valid ───────────────────────────────────────────────────────────────
 * The bootloader boots a freshly-OTA'd slot as PENDING_VERIFY and reverts to
 * the other slot on the next boot unless the app confirms itself. This task is
 * that confirmation.
 *
 * The 30 s wait is the whole design decision. Marking immediately would only
 * catch an image that dies before serving — but the failure that actually
 * strands a board is the one that boots, comes up, and panics seconds later
 * once the drive loop or MQTT session starts: a reboot cycle, marked valid on
 * every pass, that no rollback ever fires on. Waiting until the image has
 * simply survived a while catches that whole class.
 *
 * The cost, stated honestly: a power cut inside the window is indistinguishable
 * from a crash, so the board reverts to the previous image. That is the safe
 * direction to be wrong in — the old image is known good — and the window is
 * 30 s of a boot, not of a class. */
static void mark_valid_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_SELFTEST_MS));
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK)
        ESP_LOGI(TAG, "self-test passed — this image is now the rollback target");
    else
        ESP_LOGW(TAG, "mark valid failed: %s", esp_err_to_name(e));
    vTaskDelete(NULL);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the HTTP response flush + socket close */
    ESP_LOGW(TAG, "rebooting into the pushed image");
    esp_restart();
}

/* ── CORS ─────────────────────────────────────────────────────────────────────
 * The dashboard is served BY THE HUB and pushes to a rover's own IP, so every
 * browser-driven update is cross-origin. Basic auth is what forces a preflight:
 * Authorization is not a CORS-safelisted header, so the browser sends OPTIONS
 * first and never sends the 1.3 MB body unless this answers. Without it the
 * update button fails in the console with the board completely untouched — it
 * never hears about the request at all.
 *
 * Origin is echoed as * to match /fleet and /log: there are no cookies and no
 * ambient authority here, the credential is an explicit header the pusher must
 * already know, so a narrower origin would gate nothing an attacker on this
 * Wi-Fi could not do with curl. */
static void cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

static esp_err_t ota_options(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ── the push ─────────────────────────────────────────────────────────────── */
static esp_err_t ota_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    /* On every path below, including the failures: a CORS-less error response
     * is unreadable to the page, which turns "wrong password" into an opaque
     * network error and the operator into a guesser. */
    cors_headers(req);

    if (!basic_auth_ok(req)) {
        /* The realm prompt is what makes a browser ask; curl -u works either
         * way. Logged without the attempt's credentials, ever. */
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"hub\"");
        ESP_LOGW(TAG, "rejected an /ota push: bad or missing operator auth");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"operator auth required\"}");
    }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        /* Single-slot table flashed under a two-slot build, i.e. a board that
         * never got the repartition. Say so — "no OTA slot" is diagnosable,
         * a generic 500 is not. */
        httpd_resp_set_status(req, "500 Internal Server Error");
        ESP_LOGE(TAG, "no OTA slot — is this board still on a factory-only partition table?");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no OTA slot on this board\"}");
    }
    if (req->content_len <= 0 || req->content_len > dst->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        ESP_LOGW(TAG, "refused %d byte image (slot %s is %" PRIu32 ")",
                 (int)req->content_len, dst->label, dst->size);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"image missing or too big for the slot\"}");
    }

    ESP_LOGW(TAG, "push accepted: %d bytes -> %s", (int)req->content_len, dst->label);

    /* OTA_WITH_SEQUENTIAL_WRITES — erase as the bytes arrive, never up front.
     * This is not a tidiness preference: the up-front erase CRASHED the board.
     *
     * Both sized alternatives erase before the first recv — OTA_SIZE_UNKNOWN the
     * whole 1.9 MB slot, content_len about 1.4 MB. The first cut here passed
     * content_len, under a comment congratulating itself for avoiding exactly
     * this. On the esp32cam, which has the largest image and so the longest
     * erase, that reliably tripped the INTERRUPT WATCHDOG: the board reset
     * mid-transfer and the pusher saw a broken pipe, which reads like a refusal
     * and is actually a panic.
     *
     * Diagnosed 2026-07-16 by the board's own /log, over Wi-Fi, on a board whose
     * serial adapter was unplugged — the ring survived the reset that erased
     * everything else and named it:
     *
     *   W (29457) ota: push accepted: 1397488 bytes -> ota_1
     *   W  (1687) boot: ──── boot #3 ── reset: interrupt watchdog ────
     *
     * An earlier guess blamed httpd's LRU purge closing an idle socket. It was
     * wrong, and only the reset reason settled it. Sequential writes keep each
     * erase to the sector in front of the data, so nothing blocks long enough to
     * arm the watchdog. */
    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not open the slot\"}");
    }

    char buf[OTA_CHUNK];
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < OTA_CHUNK ? remaining : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;   /* slow client, not a dead one */
        if (r <= 0) {
            /* abort, not end: a half-written slot must never be bootable. */
            esp_ota_abort(h);
            ESP_LOGE(TAG, "transfer died with %d bytes to go — slot left untouched", remaining);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"transfer interrupted\"}");
        }
        e = esp_ota_write(h, buf, r);
        if (e != ESP_OK) {
            esp_ota_abort(h);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write failed\"}");
        }
        remaining -= r;
    }

    /* This is the gate that rejects a truncated or non-ESP32 payload: esp_ota_end
     * validates the image header and checksum. Reaching it is not success. */
    e = esp_ota_end(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            e == ESP_ERR_OTA_VALIDATE_FAILED
                ? "{\"ok\":false,\"error\":\"not a valid firmware image\"}"
                : "{\"ok\":false,\"error\":\"image did not verify\"}");
    }

    e = esp_ota_set_boot_partition(dst);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(e));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not arm the new slot\"}");
    }

    /* Answer BEFORE rebooting — the pusher needs to hear that it worked, and a
     * reset mid-response looks identical to a failed push from the other end. */
    char ok[64];
    snprintf(ok, sizeof ok, "{\"ok\":true,\"slot\":\"%s\"}", dst->label);
    httpd_resp_sendstr(req, ok);
    ESP_LOGW(TAG, "armed %s; rebooting", dst->label);
    xTaskCreate(reboot_task, "ota-reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void ota_update_start(void)
{
    httpd_handle_t s = wifi_portal_httpd();
    if (!s) {
        ESP_LOGE(TAG, "no shared :80 — /ota not registered (call after wifi_portal_start)");
        return;
    }
    httpd_uri_t u = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_uri_t o = { .uri = "/ota", .method = HTTP_OPTIONS, .handler = ota_options };
    /* BOTH counted against wifi_portal.c's max_uri_handlers — see its comment.
     * The preflight is a separate registration: this httpd matches on method as
     * well as path, so a POST-only /ota answers OPTIONS with 405 and the
     * browser stops there. */
    httpd_register_uri_handler(s, &u);
    httpd_register_uri_handler(s, &o);

    /* Only a slot the bootloader is still judging needs confirming. On a
     * USB-flashed board the state is ESP_OTA_IMG_UNDEFINED and there is nothing
     * pending, so no task is spawned. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "booted %s on trial — self-test runs for %d s",
                 run->label, OTA_SELFTEST_MS / 1000);
        xTaskCreate(mark_valid_task, "ota-verify", 3072, NULL, 4, NULL);
    }

    ESP_LOGI(TAG, "firmware push at POST /ota (operator auth), running from %s",
             run ? run->label : "?");
}
