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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "camera";

/* OV2640 pin map — board-specific, selected by a build flag. The data/clock lines
 * are physically wired on the module, so the wrong map ISN'T a compile error, it's
 * a camera that silently never inits (fb_get NULL). Only the SCCB pair is probeable
 * (sccb_postmortem), which is why the map is pinned per board, not auto-detected. */
#if defined(CAM_PINS_FREENOVE_S3)
/* Freenove ESP32-S3-WROOM CAM — the vendor's documented layout for this board. */
#define PWDN_GPIO   -1
#define RESET_GPIO  -1
#define XCLK_GPIO   15
#define SIOD_GPIO    4
#define SIOC_GPIO    5
#define Y9_GPIO     16
#define Y8_GPIO     17
#define Y7_GPIO     18
#define Y6_GPIO     12
#define Y5_GPIO     10
#define Y4_GPIO      8
#define Y3_GPIO      9
#define Y2_GPIO     11
#define VSYNC_GPIO   6
#define HREF_GPIO    7
#define PCLK_GPIO   13
#else
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
#endif

/* ~15 fps ceiling (see stream_handler): an uncapped QVGA feed runs at the
 * sensor's ~25 fps and its frames share the 2.4 GHz radio with the robot's drive
 * commands — that airtime contention is felt directly as drive lag. 15 fps is
 * smooth for line-of-sight driving and leaves the radio for control traffic. */
#define STREAM_FRAME_GAP_MS 50

#define PART_BOUNDARY "frameboundary"
static const char *STREAM_CT  = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_SEP = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_HDR = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* The capture pipeline (sensor clocked + framebuffers + cam_task) is EXPENSIVE to
 * hold idle: with fb_count=2 / GRAB_LATEST the driver DMAs frames NON-STOP whether
 * or not anyone is watching, and on this board's non-DMA PSRAM path (the S3-CAM's
 * OV3660, the AI-Thinker's cache-workaround rev) every frame costs a CPU memcpy. On
 * a camera board that is ALSO the hub — serving the dashboard + WS-JSON bridge on
 * the same core and the same single radio — that idle cost lands as a slow dashboard
 * and dropped WS sockets (the C3-SuperMini, camera_start() a no-op, never pays it).
 * So the camera is LAZY: presence-probed once at boot so sys.cam stays truthful, then
 * torn down until a /stream client asks for it, and torn down again a short grace
 * after the last one leaves. Nothing captures while nobody is watching. */
static bool s_present = false;   /* a sensor was detected at boot — advertise the capability (sys.cam) */
static bool s_active  = false;   /* capture pipeline currently up (guarded by s_lock) */
static int  s_viewers = 0;       /* live /stream clients (guarded by s_lock) */
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_idle;
/* Keep the sensor warm this long after the last viewer leaves: the dashboard <img>
 * reconnects on any transient socket error, and paying a full re-init (worst case the
 * 3× cold-boot retry below) on every blip would thrash. */
#define CAM_IDLE_GRACE_US (5 * 1000 * 1000)

static camera_config_t make_cfg(void)
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
    return cfg;
}

/* Bring the capture pipeline up. Caller holds s_lock; idempotent while active. The
 * SCCB probe on this board can return a garbage sensor PID on a cold boot
 * (ESP_ERR_NOT_SUPPORTED with all three OV drivers enabled) — same marginal-supply
 * signature as the PSRAM 0xffffffff reads, and it clears on a later attempt once the
 * rails settle. Retry the whole init, not just the frame buffer. */
static esp_err_t cam_up_locked(void)
{
    if (s_active) return ESP_OK;
    camera_config_t cfg = make_cfg();
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
    if (e != ESP_OK) return e;

    /* Sensor tweaks from workbench's working setup: vflip per how the module is
     * mounted; awb_gain OFF keeps white-balance auto but stops the per-frame gain
     * hunting that also destabilizes the encoder. */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
#if defined(CAM_PINS_FREENOVE_S3)
        s->set_vflip(s, 0);   /* Freenove mounts the module upright */
#else
        s->set_vflip(s, 1);   /* the AI-Thinker mounts it upside-down */
#endif
        s->set_hmirror(s, 0);
        s->set_brightness(s, 1);
        s->set_saturation(s, 1);
        s->set_awb_gain(s, 0);
    }
    s_active = true;
    return ESP_OK;
}

/* Tear the pipeline down — stops the cam_task, disables the capture DMA, and frees
 * the framebuffers, so an unwatched camera costs nothing. Caller holds s_lock. */
static void cam_down_locked(void)
{
    if (!s_active) return;
    esp_camera_deinit();
    s_active = false;
}

/* Fires CAM_IDLE_GRACE_US after the last viewer leaves; releases the camera unless a
 * new viewer arrived in the meantime (which stops this timer). */
static void cam_idle_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_viewers == 0) {
        cam_down_locked();
        ESP_LOGI(TAG, "no viewers — camera released");
    }
    xSemaphoreGive(s_lock);
}

/* A /stream client arrived: cancel any pending teardown, bring the pipeline up if
 * it's down, count the viewer. Returns the init result (ESP_OK once capturing). */
static esp_err_t viewer_enter(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_idle);   /* harmless if not armed */
    esp_err_t e = cam_up_locked();
    if (e == ESP_OK) s_viewers++;
    xSemaphoreGive(s_lock);
    return e;
}

/* A /stream client left: drop the count, and arm the grace-teardown when it hits 0. */
static void viewer_exit(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_viewers > 0) s_viewers--;
    if (s_viewers == 0) esp_timer_start_once(s_idle, CAM_IDLE_GRACE_US);
    xSemaphoreGive(s_lock);
}

/* One long-lived request that pushes frames until the client (the dashboard <img>)
 * closes the tab — httpd_resp_send_chunk returns non-OK when the socket drops. The
 * camera is spun up on entry and released (after a grace) on exit, so it only ever
 * captures while this handler is live. */
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (viewer_enter() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "camera unavailable");
        return ESP_FAIL;
    }
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
    viewer_exit();   /* last one out arms the grace-teardown; the sensor stops capturing */
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
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t ta = { .callback = cam_idle_cb, .name = "cam_idle" };
    esp_timer_create(&ta, &s_idle);

    /* Boot presence probe: bring the pipeline up ONCE to confirm a sensor is really
     * on the bus (so sys.cam reflects hardware, not just the build flag), then release
     * it — capture runs only while a client is watching (see viewer_enter). Doing the
     * probe here also front-loads the cold-boot 3× retry so the first viewer doesn't
     * pay it. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = cam_up_locked();
    if (e == ESP_OK) {
        s_present = true;
        cam_down_locked();   /* don't hold an unwatched camera — that idle cost is the bug */
    }
    xSemaphoreGive(s_lock);
    if (!s_present) {
        /* Connection-first: a board whose camera won't come up must not go down with
         * it — log loudly, scan the SCCB bus for a root cause, run without a camera
         * (sys.cam stays false, so the dashboard simply omits the video). */
        ESP_LOGE(TAG, "no camera: %s — running without one", esp_err_to_name(e));
        sccb_postmortem();
        return;
    }

    /* The :81 httpd stays listening even while the sensor is torn down — it's cheap
     * (one idle listener) and it's what catches the first /stream request to spin the
     * camera back up on demand. */
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
    ESP_LOGI(TAG, "camera ready — MJPEG at http://<robot>:81/stream (QVGA, on demand)");
}

/* Reports the camera as PRESENT (probed at boot), not as currently capturing — sys.cam
 * advertises the capability so the dashboard offers the stream, and opening it is what
 * brings the sensor up. */
bool camera_running(void) { return s_present; }

#else  /* every non-camera board: link a no-op so board_run can call it unconditionally */
void camera_start(void) {}
bool camera_running(void) { return false; }
#endif
