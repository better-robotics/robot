/*
 * camera.c — OV2640 MJPEG stream for the esp32cam board (roles.h camera_start).
 *
 * The whole file is behind HAS_CAMERA (an esp32cam-only build flag) so the driver
 * is referenced on exactly one board; the others link camera_start() as a no-op
 * and IDF's --gc-sections drops the unused esp32-camera code. The component itself
 * is target-gated in idf_component.yml (never pulled on the C3, which has no camera
 * peripheral).
 *
 * Serves multipart/x-mixed-replace JPEG at http://<robot>:81/stream — an <img>
 * source the dashboard card embeds. :81 mirrors the workbench/ESP camera convention
 * so the same URL shape works across the fleet.
 */
#include "roles.h"
#include "esp_log.h"

#ifdef HAS_CAMERA
#include <string.h>
#include <stdio.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";
static bool s_running = false;

/* AI-Thinker ESP32-CAM (OV2640) pin map — the canonical layout for this board. */
#define PWDN_GPIO   32
#define RESET_GPIO  -1
#define XCLK_GPIO    0
#define SIOD_GPIO   26
#define SIOC_GPIO   27
#define Y9_GPIO     35
#define Y8_GPIO     34
#define Y7_GPIO     39
#define Y6_GPIO     36
#define Y5_GPIO     21
#define Y4_GPIO     19
#define Y3_GPIO     18
#define Y2_GPIO      5
#define VSYNC_GPIO  25
#define HREF_GPIO   23
#define PCLK_GPIO   22

/* ~15 fps ceiling (see stream_handler): an uncapped QVGA feed runs at the
 * sensor's ~25 fps and its frames share the 2.4 GHz radio with the robot's drive
 * commands — that airtime contention is felt directly as drive lag. 15 fps is
 * smooth for line-of-sight driving and leaves the radio for control traffic. */
#define STREAM_FRAME_GAP_MS 50

#define PART_BOUNDARY "frameboundary"
static const char *STREAM_CT  = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_SEP = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_HDR = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* One long-lived request that pushes frames until the client (the dashboard <img>)
 * closes the tab — httpd_resp_send_chunk returns non-OK when the socket drops. */
static esp_err_t stream_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, STREAM_CT);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char part[80];
    esp_err_t res = ESP_OK;
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { ESP_LOGW(TAG, "frame capture failed"); res = ESP_FAIL; break; }
        int hlen = snprintf(part, sizeof part, STREAM_HDR, (unsigned)fb->len);
        if ((res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP))) == ESP_OK &&
            (res = httpd_resp_send_chunk(req, part, hlen)) == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;   /* client closed the stream */
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_GAP_MS));   /* pace to ~15 fps — frees drive airtime */
    }
    return res;
}

/* Post-mortem when the sensor probe fails: scan the SCCB bus ourselves and log
 * who ACKs. Splits the two field failures apart — no ACK at all = ribbon cable
 * unseated / sensor unpowered; an ACK the driver still rejected = sensor answered
 * with a garbage or unknown PID (marginal supply, or a sensor variant missing
 * from sdkconfig). The driver logs neither case distinctly. */
#include "driver/i2c_master.h"
static void sccb_postmortem(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,                /* any free controller — the driver's own is gone after deinit */
        .sda_io_num = SIOD_GPIO, .scl_io_num = SIOC_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "postmortem: SCCB pins still held, can't scan");
        return;
    }
    int acks = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            ESP_LOGW(TAG, "postmortem: device ACKs at 0x%02x%s", a,
                     a == 0x30 ? " (OV2640 range)" : a == 0x3c ? " (OV3660/OV5640 range)" : "");
            acks++;
        }
    }
    if (!acks)
        ESP_LOGW(TAG, "postmortem: NOTHING on the SCCB bus — check the camera ribbon cable / module power");
    i2c_del_master_bus(bus);
}

/* A bare / on :81 → /stream, so http://<robot>:81 typed by hand also lands on the feed. */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/stream");
    return httpd_resp_send(req, NULL, 0);
}

void camera_start(void)
{
    camera_config_t cfg = {
        .pin_pwdn = PWDN_GPIO, .pin_reset = RESET_GPIO, .pin_xclk = XCLK_GPIO,
        .pin_sccb_sda = SIOD_GPIO, .pin_sccb_scl = SIOC_GPIO,
        .pin_d7 = Y9_GPIO, .pin_d6 = Y8_GPIO, .pin_d5 = Y7_GPIO, .pin_d4 = Y6_GPIO,
        .pin_d3 = Y5_GPIO, .pin_d2 = Y4_GPIO, .pin_d1 = Y3_GPIO, .pin_d0 = Y2_GPIO,
        .pin_vsync = VSYNC_GPIO, .pin_href = HREF_GPIO, .pin_pclk = PCLK_GPIO,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .grab_mode = CAMERA_GRAB_LATEST,  /* freshest frame */
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* QVGA @ q=18 — the config proven on this exact board in workbench
         * (firmware/esp32_robot_idf/main/camera.c). This ESP32 rev runs the PSRAM
         * cache workaround, so the camera can't DMA into PSRAM ("PSRAM DMA mode
         * disabled") and captures via internal line buffers; VGA/low-q overruns
         * them and corrupts the JPEG (NO-SOI → fb_get NULL → 0-byte stream). QVGA
         * @ q=18 keeps frames ~5-15 KB, well within the non-DMA path. */
        .frame_size = FRAMESIZE_QVGA,     /* 320x240 */
        .jpeg_quality = 18,               /* 0-63, HIGHER = more compression = smaller/stabler */
        .fb_count = 2,                    /* double-buffer: capture N+1 while the pump sends N */
    };
    /* The SCCB probe on this board can return a garbage sensor PID on a cold
     * boot (ESP_ERR_NOT_SUPPORTED with all three OV drivers enabled) — same
     * marginal-supply signature as the PSRAM 0xffffffff reads, and it clears
     * on a later attempt once the rails settle. Retry the whole init, not
     * just the frame buffer. */
    esp_err_t e = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        e = esp_camera_init(&cfg);
        if (e == ESP_ERR_NO_MEM) {
            /* PSRAM absent or dead (this board's has read back 0xffffffff under
             * marginal power) — the driver has no DRAM fallback of its own, and
             * QVGA @ q=18 JPEG frames (~5-15 KB ×2) fit internal RAM fine. */
            ESP_LOGW(TAG, "PSRAM frame buffer failed — retrying in internal RAM");
            cfg.fb_location = CAMERA_FB_IN_DRAM;
            e = esp_camera_init(&cfg);
        }
        if (e == ESP_OK) break;
        ESP_LOGW(TAG, "esp_camera_init attempt %d/3 failed: %s", attempt, esp_err_to_name(e));
        esp_camera_deinit();   /* harmless INVALID_STATE if init never got that far */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (e != ESP_OK) {
        /* Connection-first: a camera that can't fit its buffers must not take the
         * board down — log loudly and run without it (sys.cam stays false). */
        ESP_LOGE(TAG, "esp_camera_init failed: %s — running without a camera", esp_err_to_name(e));
        sccb_postmortem();
        return;
    }

    /* Sensor tweaks from workbench's working setup: vflip because the AI-Thinker
     * mounts the module upside-down; awb_gain OFF keeps white-balance auto but stops
     * the per-frame gain hunting that also destabilizes the encoder. */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        s->set_brightness(s, 1);
        s->set_saturation(s, 1);
        s->set_awb_gain(s, 0);
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 81;
    hc.ctrl_port = 32770;             /* distinct from portal :80 (32768) + WS :9001 (32769) */
    hc.max_open_sockets = 3;
    hc.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "camera httpd (:81) failed to start");
        return;
    }
    httpd_uri_t u_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    httpd_uri_t u_root   = { .uri = "/",       .method = HTTP_GET, .handler = root_handler };
    httpd_register_uri_handler(srv, &u_stream);
    httpd_register_uri_handler(srv, &u_root);
    s_running = true;
    ESP_LOGI(TAG, "camera up — MJPEG at http://<robot>:81/stream (QVGA)");
}

bool camera_running(void) { return s_running; }

#else  /* every non-camera board: link a no-op so board_run can call it unconditionally */
void camera_start(void) {}
bool camera_running(void) { return false; }
#endif
