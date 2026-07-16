#include <stdio.h>
#include <stdlib.h>
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
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "rover_config.h"
#include "provisioning_util.h"   /* rover_format_robot_id + locator validation */
#include "roles.h"

static const char *TAG = "rover";
static char s_id[16];

/*
 * The DRIVE CLIENT of the unified image: esp-mqtt + L298N motors, with NO Wi-Fi
 * setup of its own. board_run (hub_role.c) brings the radio up in APSTA and hands
 * this a broker URI — the hub's gateway in a classroom, or mqtt://127.0.0.1:1883
 * when the board is its own island. rover_client_run connects, drives, and
 * returns on a dead session (never reboots — the caller re-evaluates).
 *
 * (BLE provisioning was removed 2026-07-09 — a rover auto-joins the open hub-*
 * AP and is named post-join from the dashboard; the specific-network case it
 * uniquely covered now rides the #17 per-board Wi-Fi config panel.)
 */

/* ── BOOT button: hold ~1 s to reboot (recover a wedged rover / force a rescan) ── */

#ifndef BUTTON_GPIO
#define BUTTON_GPIO GPIO_NUM_0   /* classic devkit BOOT; C3 SuperMini env passes GPIO_NUM_9 */
#endif

/* ── onboard LED: lit while connected to the broker ──────────────────────── */

#ifndef LED_GPIO
#define LED_GPIO GPIO_NUM_2      /* classic devkit LED; C3 SuperMini env passes GPIO_NUM_8 */
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0         /* SuperMini's LED sinks into the pin — env passes 1 */
#endif

/* Visible liveness: LED on = the rover reached the hub broker (set from the
 * MQTT CONNECTED/DISCONNECTED events). A student sees it come on when the rover
 * is live and drivable — the feedback the provisioning LED used to give. */
static void led_set(bool on) {
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? !on : on);
}

static void button_task(void *p) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held = 0;
    for (;;) {
        held = (gpio_get_level(BUTTON_GPIO) == 0) ? held + 1 : 0;
        if (held >= 10) {
            ESP_LOGI(TAG, "button held — rebooting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Start the recover button (hold to reboot). Called by board_run (hub_role.c),
 * which owns the Wi-Fi bring-up; this file is just the drive client now. */
void rover_button_start(void) {
#ifdef HAS_CAMERA
    /* GPIO0 (BUTTON_GPIO, the classic BOOT pin) is the camera XCLK on the esp32cam.
     * The running 20 MHz clock reads as a held button and reboot-loops the board.
     * The AI-Thinker CAM has no user button anyway — skip it; recover via reflash. */
    ESP_LOGI(TAG, "recover button disabled (GPIO0 is the camera XCLK on this board)");
#else
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);
#endif
}

/* ── the esp-mqtt drive client ───────────────────────────────────────────── */

/* Topic identity — a name, not a credential (confirmed 2026-07-13; CONTRACT.md
 * § Discovery & isolation): the rover publishes under robots/<name>, and every
 * hub admits every name with no MQTT auth at all — the hub's own Wi-Fi is the
 * classroom's real boundary. The MAC-derived s_id stays a payload field —
 * hardware is metadata, never the topic id.
 * Fresh boards land in the UNASSIGNED POOL: a shared name every board starts
 * with, so "factory-new" reads the same on every card until a name is
 * assigned post-join over robots/<id>/cmd/config. */
/* Identity is NVS-backed: a name assigned post-join overrides this
 * compile-time default. s_topic_id names the live buffer so publish/subscribe
 * topics follow a reassignment after the next boot. */
/* Double-buffered so a live rename can't be read half-written. config_apply
 * runs on the mqtt event task; the sys loop reads the name every 2s on
 * another. Writing one buffer in place would let that loop snapshot a
 * half-copied name and publish to robots/<garbage>/sys — and the dashboard
 * never expires a card, so one torn read would strand a phantom rover on
 * screen forever. Instead: fill the IDLE buffer, then flip s_topic_id, which
 * is a single aligned pointer store (atomic on ESP32/RISC-V). A reader either
 * sees the whole old name or the whole new one. This didn't matter while every
 * rename rebooted. */
static char s_name_buf[2][33] = { ROVER_NAME };
static volatile const char *s_topic_id_v = s_name_buf[0];
#define s_topic_id ((const char *)s_topic_id_v)

static volatile bool s_mqtt_up = false;   /* live session; drives dead-session self-heal */
static volatile int64_t s_last_drive_us = INT64_MIN;  /* esp_timer time of the last pwm; hub_watch reads it */
static volatile bool s_estop = false;     /* fleet/estop latch — non-zero pwm refused while set */

int64_t rover_ms_since_drive(void) {
    if (s_last_drive_us == INT64_MIN) return INT64_MAX;   /* no drive command this boot */
    return (esp_timer_get_time() - s_last_drive_us) / 1000;
}

/* ── motor drive: L298N, 6-wire (ENA/ENB = PWM speed, IN1-4 = direction) ──────
 * The standard L298N control: each motor's IN pair picks direction (one HIGH,
 * one LOW), its EN pin PWMs the speed. Signed ±255 per wheel from
 * robots/<id>/pwm → sign sets the IN pair, magnitude the EN duty; 0 = coast.
 * All six pins come from build_flags — set them to your wiring. A watchdog stops
 * the motors duration_ms after the last command, so a dropped session or
 * controller coasts to a halt instead of running away. */
#ifndef MOTOR_ENA
#define MOTOR_ENA 25   /* left  speed (PWM enable) — D25 */
#endif
#ifndef MOTOR_IN1
#define MOTOR_IN1 26   /* left  direction A — D26 */
#endif
#ifndef MOTOR_IN2
#define MOTOR_IN2 27   /* left  direction B — D27 */
#endif
#ifndef MOTOR_ENB
#define MOTOR_ENB 14   /* right speed (PWM enable) — D14 */
#endif
#ifndef MOTOR_IN3
#define MOTOR_IN3 12   /* right direction A — D12 (also MTDI boot strap: must be
                        * LOW at boot; the L298N input keeps it there) */
#endif
#ifndef MOTOR_IN4
#define MOTOR_IN4 13   /* right direction B — D13 */
#endif

/* Wheel encoders — wired D32 / D34, single-channel (direction inferred from the
 * motor sign). Not yet read: odometry + PID is the next feature (duke#18).
 * D34 is input-only with no internal pull, so it needs a push-pull encoder or
 * an external pull-up when that lands. */
#define ENCODER_LEFT  32
#define ENCODER_RIGHT 34

/* Runtime pins — default to the macros above, overridable post-join via
 * robots/<id>/cmd/config {"pins":{ena,in1,in2,enb,in3,in4}} → NVS. A student
 * wires their own chassis, so the pinout is config, not a build constant.
 * Encoders stay compile-time: nothing reads them yet (odometry is duke#18), so
 * a config field for them would claim a capability that isn't wired. */
static int s_pin_ena = MOTOR_ENA, s_pin_in1 = MOTOR_IN1, s_pin_in2 = MOTOR_IN2,
           s_pin_enb = MOTOR_ENB, s_pin_in3 = MOTOR_IN3, s_pin_in4 = MOTOR_IN4;
static bool s_motor_ready = false;   /* false if init failed on a bad pin — drive is a no-op */

enum { CH_ENA, CH_ENB };   /* two LEDC channels, one per enable/speed pin */
static esp_timer_handle_t s_motor_watchdog;

/* Safety floor (CONTRACT.md § Safety floor): a non-zero drive ALWAYS self-
 * expires. Without the clamp, duration_ms<=0 skips the watchdog arm (motors
 * run until the next command — forever, if none comes) and an oversized value
 * defeats it in practice. 4000 matches workbench's LLM_MAX_DURATION_MS, so a
 * single command from any client is bounded the same everywhere; a sustained
 * drive is a refreshing command stream, the human-joystick shape. */
#define PWM_DEFAULT_DURATION_MS 400
#define PWM_MAX_DURATION_MS    4000

static void motor_speed(int ch, int duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty < 0 ? 0 : duty > 255 ? 255 : duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

/* signed ±255 per wheel: sign → the IN direction pair, magnitude → EN duty. */
static void motor_drive(int left, int right) {
    if (!s_motor_ready) return;   /* a bad pin config left motors uninitialized */
    gpio_set_level(s_pin_in1, left > 0);
    gpio_set_level(s_pin_in2, left < 0);
    motor_speed(CH_ENA, left  < 0 ? -left  : left);
    gpio_set_level(s_pin_in3, right > 0);
    gpio_set_level(s_pin_in4, right < 0);
    motor_speed(CH_ENB, right < 0 ? -right : right);
}

static void motor_stop(void *unused) { (void)unused; motor_drive(0, 0); }

/* Graceful, not ESP_ERROR_CHECK: a student-supplied pin could be invalid for
 * this chip, and aborting would reboot → re-read the same pin → abort again, a
 * permanent boot loop. On failure the motors stay a no-op (s_motor_ready=false)
 * and the rover still boots, connects, and reports — recoverable by re-config. */
static void motor_init(void) {
#ifdef HAS_CAMERA
    /* The AI-Thinker CAM is pin-starved — the OV3660 claims ~16 GPIOs and the XCLK
     * uses LEDC_TIMER_0, the same timer the motor PWM reconfigures. Initing motors
     * here reprograms that timer (killing the camera clock) and can stomp camera
     * GPIOs. The CAM is a camera platform, not a drive platform — skip motors. */
    ESP_LOGI(TAG, "motors disabled on the camera board (pins + LEDC belong to the OV3660)");
    return;
#endif
    gpio_config_t dirs = {
        .pin_bit_mask = (1ULL << s_pin_in1) | (1ULL << s_pin_in2) |
                        (1ULL << s_pin_in3) | (1ULL << s_pin_in4),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t e = gpio_config(&dirs);
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT, .freq_hz = 1000, .clk_cfg = LEDC_AUTO_CLK,
    };
    if (e == ESP_OK) e = ledc_timer_config(&timer);
    const int en[2] = { s_pin_ena, s_pin_enb };
    for (int ch = 0; ch < 2 && e == ESP_OK; ch++) {
        ledc_channel_config_t c = {
            .gpio_num = en[ch], .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = ch, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
        };
        e = ledc_channel_config(&c);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "motor init failed (%s) — bad pin config? motors disabled",
                 esp_err_to_name(e));
        return;
    }
    s_motor_ready = true;
    motor_drive(0, 0);   /* enables low, direction defined */
    const esp_timer_create_args_t wd = { .callback = motor_stop, .name = "motor_wd" };
    ESP_ERROR_CHECK(esp_timer_create(&wd, &s_motor_watchdog));
    ESP_LOGI(TAG, "motors: L ena=%d in=%d/%d  R enb=%d in=%d/%d",
             s_pin_ena, s_pin_in1, s_pin_in2, s_pin_enb, s_pin_in3, s_pin_in4);
}

static int json_int(const cJSON *root, const char *key, int dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

/* One robots/<id>/pwm command: {left_motor,right_motor,duration_ms}. Re-arms the
 * watchdog each message, so the drive self-expires duration_ms after the last
 * command — the safety floor if commands stop arriving mid-motion. */
static void motor_apply(const char *json, int len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { ESP_LOGW(TAG, "pwm: unparseable payload"); return; }
    /* Optional "target" board-id, same filter as cmd/config: pool (and
     * collision) boards all see one pwm topic, so a target drives ONE of
     * them; absent = every subscriber drives (several boards sharing one
     * name's normal case). */
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (cJSON_IsString(target) && strcmp(target->valuestring, s_id) != 0) {
        cJSON_Delete(root); return;   /* not addressed to this board */
    }
    int left  = json_int(root, "left_motor",  0);
    int right = json_int(root, "right_motor", 0);
    int ms    = json_int(root, "duration_ms", PWM_DEFAULT_DURATION_MS);
    cJSON_Delete(root);

    if (left || right) {                              /* zero drive = stop; no timer needed */
        if (ms <= 0) ms = PWM_DEFAULT_DURATION_MS;
        else if (ms > PWM_MAX_DURATION_MS) ms = PWM_MAX_DURATION_MS;
    }
    /* Fleet e-stop latch (CONTRACT.md § Fleet e-stop): while engaged, every
     * non-zero drive is refused — above the per-command self-expiry, which
     * only bounds commands that were accepted. Zero drive is always honored. */
    if (s_estop && (left || right)) {
        motor_stop(NULL);
        ESP_LOGW(TAG, "pwm refused — fleet e-stop engaged");
        return;
    }
    s_last_drive_us = esp_timer_get_time();           /* hub_watch skips its scan while driving */
    motor_drive(left, right);
    esp_timer_stop(s_motor_watchdog);                 /* no-op if not armed */
    if (ms > 0) esp_timer_start_once(s_motor_watchdog, (int64_t)ms * 1000);
    ESP_LOGI(TAG, "pwm L=%d R=%d for %d ms", left, right, ms);
}

/* fleet/estop — the room-wide retained latch (CONTRACT.md § Fleet e-stop),
 * above the per-command self-expiry. Retained delivery on subscribe means a
 * rover that reboots or rejoins mid-emergency latches anyway. Empty payload =
 * clear (the delete-retained idiom); an unparseable payload = ENGAGE — parse
 * failure fails toward stopped. */
static void estop_apply(const char *json, int len) {
    bool engaged = true;
    if (len == 0) {
        engaged = false;
    } else {
        cJSON *root = cJSON_ParseWithLength(json, len);
        if (root) {   /* missing "engaged" reads as engaged, same fail-toward-stopped bias */
            engaged = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "engaged"));
            cJSON_Delete(root);
        }
    }
    if (engaged == s_estop) return;
    s_estop = engaged;
    if (engaged) {
        motor_stop(NULL);
        ESP_LOGW(TAG, "fleet E-STOP engaged — drive refused until cleared");
    } else {
        ESP_LOGW(TAG, "fleet e-stop cleared — drive re-enabled");
    }
}

/* Post-join assignment: {"name":"scout"} on robots/<id>/cmd/config. No password
 * rides along; a name is an address, not a credential. This is the reshaped
 * onboarding — the hub dashboard assigns a rover instead of BLE/compile flags.
 *
 * A NAME CHANGE NO LONGER REBOOTS (2026-07-16). It used to, but only because
 * this function batched four settings behind one `changed` flag and pins/flip
 * genuinely need a restart (motor_init already configured the LEDC channels
 * with the old map). The name was paying someone else's cost — on the most
 * common classroom action, for a ~10s dropout. A name is just the topic we
 * subscribe under, so re-pointing the subscriptions IS the rename. Same for
 * the hub pin, which is only ever read at discovery/hub-watch.
 *
 * Publishes follow because the sys loop rebuilds its topic from s_topic_id
 * every tick (s_topic_id names the live name buffer). It used to snapshot it
 * once before the loop — fine while every rename rebooted, and a silent
 * stale-card bug the moment one didn't. */
static void config_apply(const char *json, int len, esp_mqtt_client_handle_t cli) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { ESP_LOGW(TAG, "config: unparseable payload"); return; }
    /* Optional "target" board-id: rovers sharing a name all see this topic, so a
     * target lets the dashboard assign ONE of them; absent = applies to all. */
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (cJSON_IsString(target) && strcmp(target->valuestring, s_id) != 0) {
        cJSON_Delete(root); return;   /* not addressed to this board */
    }
    bool changed = false;      /* anything at all — worth logging */
    bool needs_boot = false;   /* ONLY pins/flip: motor_init already ran */
    char old_name[33] = "";    /* set iff the name moved, to unsubscribe from */

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name) && name->valuestring[0]
        && strcmp(name->valuestring, s_topic_id) != 0) {   /* same name = no-op, not churn */
        rover_config_set_identity(name->valuestring);
        snprintf(old_name, sizeof old_name, "%s", s_topic_id);   /* what to unsubscribe from */
        /* Fill the idle buffer, THEN publish it with one pointer store. */
        char *idle = (s_topic_id == s_name_buf[0]) ? s_name_buf[1] : s_name_buf[0];
        snprintf(idle, sizeof s_name_buf[0], "%s", name->valuestring);
        s_topic_id_v = idle;
        ESP_LOGW(TAG, "assigned name '%s' (was '%s')", s_topic_id, old_name);
        changed = true;
    }

    /* Optional "hub" pin — lock this board to one exact hub SSID (rogue-hub
     * guard, TOFU at assignment time). "" clears; a non-hub-* value is refused
     * (it would admit nothing and strand the board off every hub). */
    const cJSON *hub = cJSON_GetObjectItemCaseSensitive(root, "hub");
    if (cJSON_IsString(hub)) {
        if (rover_config_set_hub_pin(hub->valuestring) == ESP_OK) {
            ESP_LOGW(TAG, "hub pin %s%s", hub->valuestring[0] ? "set: " : "cleared",
                     hub->valuestring);
            changed = true;
        } else {
            ESP_LOGW(TAG, "config: hub pin '%s' refused (must be hub-* or empty)",
                     hub->valuestring);
        }
    }

    /* Optional {"pins":{ena,in1,in2,enb,in3,in4}} — a student's chassis wiring.
     * Seed with current pins so a partial object only overrides what it names;
     * set_motor_pins rejects out-of-range and won't persist a boot-loop pin. */
    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(root, "pins");
    if (cJSON_IsObject(pins)) {
        int p[6] = { s_pin_ena, s_pin_in1, s_pin_in2, s_pin_enb, s_pin_in3, s_pin_in4 };
        const char *keys[6] = { "ena", "in1", "in2", "enb", "in3", "in4" };
        for (int i = 0; i < 6; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(pins, keys[i]);
            if (cJSON_IsNumber(v)) p[i] = v->valueint;
        }
        if (rover_config_set_motor_pins(p) == ESP_OK) {
            needs_boot = true;   /* motor_init ran with the old map */
            ESP_LOGW(TAG, "motor pins updated: %d %d %d %d %d %d",
                     p[0], p[1], p[2], p[3], p[4], p[5]);
            changed = true;
        } else {
            ESP_LOGW(TAG, "config: motor pins out of range (0..33), ignored");
        }
    }

    /* Optional {"flip":{"left"|"right"|"swap":true}} — wrong drive directions
     * are a wiring permutation, so they're fixed as one: left = swap IN1/IN2,
     * right = swap IN3/IN4, swap = exchange the whole L/R triples. Sent by the
     * dashboard's motor-orientation buttons; persists exactly like pins. */
    const cJSON *flip = cJSON_GetObjectItemCaseSensitive(root, "flip");
    if (cJSON_IsObject(flip)) {
        int p[6] = { s_pin_ena, s_pin_in1, s_pin_in2, s_pin_enb, s_pin_in3, s_pin_in4 };
        int tmp;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(flip, "left"))) {
            tmp = p[1]; p[1] = p[2]; p[2] = tmp;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(flip, "right"))) {
            tmp = p[4]; p[4] = p[5]; p[5] = tmp;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(flip, "swap"))) {
            for (int i = 0; i < 3; i++) { tmp = p[i]; p[i] = p[i + 3]; p[i + 3] = tmp; }
        }
        if (rover_config_set_motor_pins(p) == ESP_OK) {
            needs_boot = true;   /* motor_init ran with the old map */
            ESP_LOGW(TAG, "motor orientation flip -> pins %d %d %d %d %d %d",
                     p[0], p[1], p[2], p[3], p[4], p[5]);
            changed = true;
        }
    }

    cJSON_Delete(root);

    /* A rename is just a change of address: drop the old subscriptions and take
     * the new ones. Order matters — subscribe BEFORE unsubscribing, so a pwm
     * command arriving mid-rename lands on one of them rather than neither.
     * (The reverse order leaves a hole where the rover answers to no topic.)
     * Doing this needs no reboot, which is the whole point; only pins/flip do. */
    if (old_name[0] && cli) {
        char t[64];
        static const char *chans[] = { "pwm", "cmd/config", "cmd/identify", "cmd/reprovision" };
        for (size_t i = 0; i < sizeof chans / sizeof *chans; i++) {
            snprintf(t, sizeof t, "robots/%s/%s", s_topic_id, chans[i]);
            esp_mqtt_client_subscribe(cli, t, 0);
        }
        for (size_t i = 0; i < sizeof chans / sizeof *chans; i++) {
            snprintf(t, sizeof t, "robots/%s/%s", old_name, chans[i]);
            esp_mqtt_client_unsubscribe(cli, t);
        }
        ESP_LOGW(TAG, "re-subscribed as '%s' — no reboot", s_topic_id);
    }

    if (needs_boot) {   /* pins/flip only: motor_init must re-run against NVS */
        ESP_LOGW(TAG, "motor map changed — rebooting to re-init");
        vTaskDelay(pdMS_TO_TICKS(300));   /* flush log + let the broker ack */
        esp_restart();
    }
    if (!changed) ESP_LOGW(TAG, "config: nothing to apply");
}

/* ── cmd/identify: blink the LED so a physical board can be matched to its id ──
 * The assign flow's missing physical link: the instructor sees rover-c9d0 on
 * screen and three identical boards on the desk. ~6 s of 2 Hz blinking, then the
 * LED returns to its liveness meaning (on = connected to the broker). */
static volatile bool s_blinking = false;

static void blink_task(void *p) {
    (void)p;
    for (int i = 0; i < 24; i++) {   /* 24 half-periods × 250 ms = 6 s at 2 Hz */
        led_set(i & 1);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    led_set(s_mqtt_up);
    s_blinking = false;
    vTaskDelete(NULL);
}

static void identify_apply(const char *json, int len) {
    /* Same optional "target" rule as config: boards sharing a name all see this
     * topic; a target picks ONE. Empty/unparseable payload blinks (forgiving —
     * identify is a lamp, not a mutation). */
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root) {
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
        bool skip = cJSON_IsString(target) && strcmp(target->valuestring, s_id) != 0;
        cJSON_Delete(root);
        if (skip) return;   /* addressed to a different board sharing this name */
    }
    if (s_blinking) return;
    s_blinking = true;
    ESP_LOGI(TAG, "identify — blinking the LED");
    xTaskCreate(blink_task, "blink", 2048, NULL, 4, NULL);
}

/* Reprovision: identity destruction. It arrives on this board's own name
 * topic — anyone who can address robots/<name>/cmd/reprovision can send it,
 * same openness as every other command topic now. Optional
 * {"target":"rover-xxxx"} narrows a name-wide publish to one board, the same
 * filter cmd/config uses; no/unparseable payload = every board sharing that
 * name. */
static void reprovision_apply(const char *json, int len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root) {
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
        bool mine = !cJSON_IsString(target) || strcmp(target->valuestring, s_id) == 0;
        cJSON_Delete(root);
        if (!mine) return;
    }
    ESP_LOGW(TAG, "reprovision — clearing identity, rebooting into the pool");
    rover_config_clear_identity();
    esp_restart();
}

/* Four subscriptions: robots/<id>/pwm (drive), /cmd/config (post-join name
 * assignment), /cmd/identify (blink to find the physical board), /cmd/reprovision
 * (the BOOT button's remote twin). Routed by topic suffix; topic/data arrive
 * without null terminators. */
static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_mqtt_up = true;
        led_set(true);          /* reached the broker — visible "live" signal */
        char t[64];
        snprintf(t, sizeof t, "robots/%s/pwm", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        snprintf(t, sizeof t, "robots/%s/cmd/config", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        snprintf(t, sizeof t, "robots/%s/cmd/identify", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        snprintf(t, sizeof t, "robots/%s/cmd/reprovision", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        /* Resync the e-stop latch from broker state: the retained fleet/estop
         * (if any) arrives with this subscribe and re-engages. No retained
         * message = the room is clear (a hub restart forgets an engaged stop
         * by design — CONTRACT.md § Fleet e-stop), so drop any stale latch
         * rather than stranding the rover stopped with nothing to clear it. */
        s_estop = false;
        esp_mqtt_client_subscribe(e->client, "fleet/estop", 1);
        ESP_LOGI(TAG, "mqtt connected; subscribed pwm + cmd/{config,identify,reprovision} for %s + fleet/estop", s_topic_id);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        led_set(false);                                /* off the broker → not live */
        motor_stop(NULL);                              /* lose the broker → stop moving */
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (e->topic_len == 11 && memcmp(e->topic, "fleet/estop", 11) == 0) {
            estop_apply(e->data, e->data_len);
        } else if (e->topic_len >= 4 && memcmp(e->topic + e->topic_len - 4, "/pwm", 4) == 0) {
            motor_apply(e->data, e->data_len);
        } else if (e->topic_len >= 7 && memcmp(e->topic + e->topic_len - 7, "/config", 7) == 0) {
            config_apply(e->data, e->data_len, e->client);
        } else if (e->topic_len >= 9 && memcmp(e->topic + e->topic_len - 9, "/identify", 9) == 0) {
            identify_apply(e->data, e->data_len);
        } else if (e->topic_len >= 12 && memcmp(e->topic + e->topic_len - 12, "/reprovision", 12) == 0) {
            reprovision_apply(e->data, e->data_len);
        } else {
            /* A future subscription without a branch here must never reboot the board. */
            ESP_LOGW(TAG, "unhandled message on %.*s — ignoring", e->topic_len, e->topic);
        }
        break;
    default:
        /* No MQTT_EVENT_ERROR handling: a rover never claims to be
         * "instructor", so no hub ever refuses its connection — nothing to
         * count or fall back from. */
        break;
    }
}

/* ── rover_client_run: the MQTT client + motor-drive loop, network-agnostic ───
 * Assumes Wi-Fi/networking is already up and `broker_uri` reaches a broker; sets
 * up identity + motors, connects, then blocks in the publish loop. Its sole
 * caller is board_run (hub_role.c), which brings the radio up in APSTA and passes
 * the right broker: the DHCP gateway when joined to a hub (classroom), or
 * mqtt://127.0.0.1:1883 when the board is its own island (home). Returns (never
 * reboots) when the session is dead — board_run re-evaluates in its loop. */
void rover_client_run(const char *broker_uri) {
    /* Identity is board-local, no network needed: robot-id from the MAC, name
     * from NVS (a post-join assignment) or the compile-time pool default. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    rover_format_robot_id(mac, s_id);
    char cu[33];
    rover_config_load_identity(cu);
    if (cu[0]) snprintf(s_name_buf[0], sizeof s_name_buf[0], "%s", cu);   /* boot: no reader yet */
    led_set(false);   /* start dark; MQTT CONNECTED lights it */
    ESP_LOGI(TAG, "rover client: id %s as '%s' → %s", s_id, s_topic_id, broker_uri);

    /* No .credentials: every hub admits every name with no MQTT auth at all
     * (confirmed 2026-07-13 — CONTRACT.md § Discovery & isolation). MQTT is
     * still the transport for an unrelated, still-true reason: zenoh-pico
     * never implemented usrpwd, which mattered when identity WAS a
     * credential; esp-mqtt was simply the client that worked. Same firmware
     * reaches either hub: both are raw-TCP brokers on :1883, and a rover
     * never needs the WebSocket transport (that's the browser's constraint). */
    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = broker_uri,
        .session.keepalive = 15,
    };
    {   /* pins default to the macros; NVS overrides when a chassis was configured */
        int pins[6] = { s_pin_ena, s_pin_in1, s_pin_in2, s_pin_enb, s_pin_in3, s_pin_in4 };
        if (rover_config_load_motor_pins(pins)) {
            s_pin_ena = pins[0]; s_pin_in1 = pins[1]; s_pin_in2 = pins[2];
            s_pin_enb = pins[3]; s_pin_in3 = pins[4]; s_pin_in4 = pins[5];
            ESP_LOGI(TAG, "motor pins from NVS");
        }
    }
    motor_init();   /* ready before CONNECTED can deliver a pwm command */
    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&mcfg);
    if (!cli) { ESP_LOGE(TAG, "mqtt init failed"); return; }
    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    esp_mqtt_client_start(cli);

    /* First connect gates everything: it proves reachability (there's no
     * credential left to be rejected). No connect in 10 s → dead → caller
     * retries. MUST stop+destroy on this exit: a returned-but-alive client
     * keeps auto-reconnecting forever — each pass leaked one zombie, its
     * DISCONNECTED events flipped the shared s_mqtt_up under the NEXT
     * session (20 s flap), and the heap bled until the board crashed off the
     * air (bench 2026-07-12, rover-e348/b79c). */
    for (int i = 0; i < 40 && !s_mqtt_up; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_mqtt_up) {
        ESP_LOGE(TAG, "broker unreachable");
        esp_mqtt_client_stop(cli);
        esp_mqtt_client_destroy(cli);
        return;
    }

    /* Rebuilt EVERY tick, not once here: a live rename (config_apply) moves
     * the name under this loop, and a topic snapshotted before the loop would
     * keep publishing sys to the old address forever — the dashboard would show
     * a card that never updates while the rover looked healthy on the wire.
     * A snprintf per 2s is free; the bug it prevents is not. */
    char key[48];
    ESP_LOGI(TAG, "publishing robots/%s/sys every 2 s", s_topic_id);

    char buf[320];   /* worst-case sys payload with "pins"+rssi is ~250 — keep headroom */
    int down = 0;   /* consecutive 2 s ticks with no live session — dead → return */
    for (;;) {
        if (s_mqtt_up) {
            down = 0;
            snprintf(key, sizeof key, "robots/%s/sys", s_topic_id);   /* follows a rename */
            int64_t up_ms = esp_timer_get_time() / 1000;
            uint32_t heap = esp_get_free_heap_size();
            /* STA IP so the dashboard can reach this rover's camera (:81/stream) and
             * link to it directly; empty until the uplink has a lease. */
            char ip[16] = "";
            esp_netif_ip_info_t ipi;
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr) {
                snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipi.ip));   /* classroom: STA on the hub subnet */
            } else {
                /* Island (no uplink): reachable at our own AP address instead, so a
                 * self-served dashboard can still load this board's camera. */
                esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
                if (ap && esp_netif_get_ip_info(ap, &ipi) == ESP_OK && ipi.ip.addr)
                    snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipi.ip));
            }
            /* Uplink RSSI — the dashboard's Signal chip (and the workbench
             * vitals parity CONTRACT.md already names). Only present while
             * the STA is associated: an island rover has no uplink to rate,
             * and an absent key reads as "no signal to show", not 0 dBm. */
            char rssi[24] = "";
            wifi_ap_record_t apr;
            if (esp_wifi_sta_get_ap_info(&apr) == ESP_OK)
                snprintf(rssi, sizeof rssi, ",\"rssi_dbm\":%d", apr.rssi);
            /* Latched e-stop acknowledgment — the fleet view verifies each
             * rover actually heard the retained latch. Absent = clear, the
             * same absent-key idiom as rssi. */
            const char *es = s_estop ? ",\"estop\":true" : "";
            /* board id is metadata in the payload, not the topic (id == name). */
#ifdef HAS_CAMERA
            /* No "pins" on the camera board: motors are structurally disabled
             * there (motor_init skips), so reporting a map would claim a
             * capability that doesn't exist. */
            char net[65];
            board_uplink_ssid_json(net);
            snprintf(buf, sizeof buf,
                     "{\"uptime_ms\":%lld,\"free_heap\":%u,\"hw\":\"" HW_BOARD
                     "\",\"board\":\"%s\",\"ip\":\"%s\",\"net\":\"%s\",\"cam\":%s%s%s,\"synthetic\":false}",
                     (long long)up_ms, (unsigned)heap, s_id, ip, net,
                     camera_running() ? "true" : "false", rssi, es);
#else
            /* "pins" = the live motor map (NVS-or-default, what motor_init
             * ran with) — the dashboard's pin editor shows this as truth
             * instead of blind-writing. Changes only across a reboot, so the
             * 2 s cadence is effectively instant read-back after a rejoin. */
            char net[65];
            board_uplink_ssid_json(net);
            snprintf(buf, sizeof buf,
                     "{\"uptime_ms\":%lld,\"free_heap\":%u,\"hw\":\"" HW_BOARD
                     "\",\"board\":\"%s\",\"ip\":\"%s\",\"net\":\"%s\",\"cam\":%s%s%s,\"synthetic\":false,"
                     "\"pins\":{\"ena\":%d,\"in1\":%d,\"in2\":%d,\"enb\":%d,\"in3\":%d,\"in4\":%d}}",
                     (long long)up_ms, (unsigned)heap, s_id, ip, net,
                     camera_running() ? "true" : "false", rssi, es,
                     s_pin_ena, s_pin_in1, s_pin_in2, s_pin_enb, s_pin_in3, s_pin_in4);
#endif
            /* DEBUG, not INFO — so it is compiled out at this firmware's Info
             * ceiling and reaches neither the UART nor the RTC ring.
             *
             * This is the most repetitive line in the system: ~250 bytes every
             * 2 s. At INFO it wrote ~125 B/s into device_log.c's 6 KB ring,
             * giving /log a memory of about 50 seconds — all of it this line.
             * A board up for a minute answered "why did it reboot" with nothing
             * but its own heartbeat (seen on the bench 2026-07-16: 27 log lines,
             * 27 of them this). The ring is for the events around a failure, and
             * a heartbeat is not an event.
             *
             * Nothing is lost: this duplicates the message the dashboard's
             * Messages drawer already shows, live, off the wire it was published
             * on. A publish that FAILS is still a warning, because that is an
             * event. */
            if (esp_mqtt_client_publish(cli, key, buf, 0, 0, 0) >= 0)
                ESP_LOGD(TAG, "pub %s", buf);
            else
                ESP_LOGW(TAG, "publish enqueue failed");
        } else if (++down >= 10) {
            /* esp-mqtt auto-reconnects, so a brief outage self-heals; only a
             * sustained dead session (~20 s) returns to the caller. */
            ESP_LOGE(TAG, "20 s with no broker — session dead");
            esp_mqtt_client_stop(cli);
            esp_mqtt_client_destroy(cli);
            return;
        } else {
            ESP_LOGW(TAG, "waiting for reconnect (%d/10)", down);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

