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
#include "provisioning_util.h"
#include "provisioning.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "rover";
static char s_id[16];

/*
 * Mode dispatch is stateless: NVS holds only explicit choices, and behavior
 * is a pure function of that config plus a one-shot provision request. The
 * request rides RTC noinit RAM — it survives esp_restart() (how a mode asks
 * for the other one) but not a power cycle, so no robot can ever be stranded
 * in the wrong mode by stale state.
 *
 * Nothing stored is a fully operable state: no ssid → scan-join the strongest
 * open hub-* network (the classroom AP convention IS the onboarding channel),
 * no locator → dial the network gateway (on its own AP the hub is the
 * gateway). BLE provisioning is the override path, not the front door.
 *
 * The two radios never run in the same boot:
 *
 *   operating (Wi-Fi + MQTT)  ── failure, button, or fabric command ──► provisioning window
 *   provisioning (BLE)        ── done, window expiry, or button ──► restart into operating,
 *                                which retries stored credentials or re-scans for a hub
 */
#define PROVISION_MAGIC 0x50524f56u  /* "PROV" */
static RTC_NOINIT_ATTR uint32_t s_provision_request;

#define PROVISION_WINDOW_US (3 * 60 * 1000000LL)

static volatile bool s_operating = false;  /* which mode the button acts on */

/* ── BOOT button: hold ~1 s to switch modes ──────────────────────────────── */

#ifndef BUTTON_GPIO
#define BUTTON_GPIO GPIO_NUM_0   /* classic devkit BOOT; C3 SuperMini env passes GPIO_NUM_9 */
#endif

/* ── onboard LED: lit while provisioning ─────────────────────────────────── */

#ifndef LED_GPIO
#define LED_GPIO GPIO_NUM_2      /* classic devkit LED; C3 SuperMini env passes GPIO_NUM_8 */
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0         /* SuperMini's LED sinks into the pin — env passes 1 */
#endif

/* Entering provisioning mode is otherwise invisible from the outside — the
 * first field test of the BOOT button read as "nothing happened" while the
 * window was in fact open (2026-07-04). LED on = BLE window open. */
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
            ESP_LOGI(TAG, "button held — switching to %s",
                     s_operating ? "provisioning" : "operating");
            if (s_operating) s_provision_request = PROVISION_MAGIC;
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── operating mode: Wi-Fi STA + esp-mqtt client ─────────────────────────── */

static volatile bool s_got_ip = false;
static volatile bool s_want_connect = false;   /* gates auto-reconnect during scans */
static esp_ip4_addr_t s_gw;                    /* DHCP gateway — on the hub's AP, the hub */

static void on_evt(void *a, esp_event_base_t base, int32_t id, void *d) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        if (s_want_connect) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_gw = ((ip_event_got_ip_t *)d)->ip_info.gw;
        s_got_ip = true;
    }
}

static void wifi_up(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wifi_join(const char *ssid, const char *pass) {
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_want_connect = true;
    esp_wifi_connect();
    /* Wait up to 30 s; the event handler keeps reconnecting, so a brief AP
     * outage inside this window resolves without dropping to provisioning. */
    for (int i = 0; i < 120 && !s_got_ip; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_got_ip) { s_want_connect = false; esp_wifi_disconnect(); }
    return s_got_ip;
}

/* Zero-touch onboarding: an OPEN network named hub-* is the classroom
 * convention, so its existence is all the configuration a rover needs.
 * Strongest wins when several are in range. */
static bool discover_hub(char out[33]) {
    ESP_LOGI(TAG, "scanning for an open hub-* network");
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    wifi_ap_record_t *ap = malloc(n * sizeof *ap);
    if (!ap) return false;
    esp_wifi_scan_get_ap_records(&n, ap);
    int best = -1;
    for (int i = 0; i < n; i++)
        if (ap[i].authmode == WIFI_AUTH_OPEN &&
            strncmp((const char *)ap[i].ssid, "hub-", 4) == 0 &&
            (best < 0 || ap[i].rssi > ap[best].rssi))
            best = i;
    if (best >= 0) {
        snprintf(out, 33, "%s", (const char *)ap[best].ssid);
        ESP_LOGI(TAG, "found %s (%d dBm)", out, ap[best].rssi);
    }
    free(ap);
    return best >= 0;
}

/* MQTT identity. robot-id == the team credential (CONTRACT.md § Discovery &
 * isolation): the rover authenticates as its team and publishes under its own
 * robots/<team> subtree, so the Pi's `pattern robots/%u/#` ACL admits it and a
 * team can't touch another's subtree. The MAC-derived s_id stays the BLE adv
 * name + a payload field — hardware is metadata, never the topic id.
 * Demo defaults; real per-team creds arrive via provisioning (hub#1 follow-up). */
#ifndef MQTT_USER
#define MQTT_USER "team1"
#endif
#ifndef MQTT_PASS
#define MQTT_PASS "change-me-team1"
#endif
/* Identity is NVS-backed: a team assigned post-join (robots/<id>/cmd/config)
 * overrides these compile-time defaults. s_topic_id tracks s_user so the
 * publish/subscribe topics follow a reassignment after the next boot. */
static char s_user[33] = MQTT_USER;
static char s_pass[65] = MQTT_PASS;
static char s_name[33] = "";
static const char *s_topic_id = s_user;

static volatile bool s_mqtt_up = false;   /* live session; drives dead-session self-heal */

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
    int left  = json_int(root, "left_motor",  0);
    int right = json_int(root, "right_motor", 0);
    int ms    = json_int(root, "duration_ms", 400);
    cJSON_Delete(root);

    motor_drive(left, right);
    esp_timer_stop(s_motor_watchdog);                 /* no-op if not armed */
    if (ms > 0) esp_timer_start_once(s_motor_watchdog, (int64_t)ms * 1000);
    ESP_LOGI(TAG, "pwm L=%d R=%d for %d ms", left, right, ms);
}

/* Post-join assignment: {"team":"team2","pass":"…","name":"Rover A"} on
 * robots/<id>/cmd/config. Persist and reboot to reconnect under the new identity
 * (the team changes both the topic and the credential). This is the reshaped
 * onboarding — the hub dashboard assigns a rover instead of BLE/compile flags. */
static void config_apply(const char *json, int len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) { ESP_LOGW(TAG, "config: unparseable payload"); return; }
    /* Optional "target" board-id: rovers sharing a team all see this topic, so a
     * target lets the dashboard assign ONE of them; absent = applies to all. */
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    if (cJSON_IsString(target) && strcmp(target->valuestring, s_id) != 0) {
        cJSON_Delete(root); return;   /* not addressed to this board */
    }
    bool changed = false;

    const cJSON *team = cJSON_GetObjectItemCaseSensitive(root, "team");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(team) && team->valuestring[0]) {
        rover_config_set_identity(team->valuestring,
                                  cJSON_IsString(pass) ? pass->valuestring : "",
                                  cJSON_IsString(name) ? name->valuestring : "");
        ESP_LOGW(TAG, "assigned team '%s'", team->valuestring);
        changed = true;
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
            ESP_LOGW(TAG, "motor pins updated: %d %d %d %d %d %d",
                     p[0], p[1], p[2], p[3], p[4], p[5]);
            changed = true;
        } else {
            ESP_LOGW(TAG, "config: motor pins out of range (0..33), ignored");
        }
    }

    cJSON_Delete(root);
    if (changed) {   /* reboot re-reads NVS: new team reconnects, new pins re-init */
        ESP_LOGW(TAG, "config applied — rebooting");
        vTaskDelay(pdMS_TO_TICKS(300));   /* flush log + let the broker ack */
        esp_restart();
    }
    ESP_LOGW(TAG, "config: nothing to apply");
}

/* Three subscriptions: robots/<id>/pwm (drive), /cmd/config (post-join team
 * assignment), /cmd/reprovision (the BOOT button's remote twin). Routed by topic
 * suffix; topic/data arrive without null terminators. */
static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_mqtt_up = true;
        char t[64];
        snprintf(t, sizeof t, "robots/%s/pwm", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        snprintf(t, sizeof t, "robots/%s/cmd/config", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        snprintf(t, sizeof t, "robots/%s/cmd/reprovision", s_topic_id);
        esp_mqtt_client_subscribe(e->client, t, 0);
        ESP_LOGI(TAG, "mqtt connected; subscribed pwm + cmd/{config,reprovision} for %s", s_topic_id);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        motor_stop(NULL);                              /* lose the broker → stop moving */
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (e->topic_len >= 4 && memcmp(e->topic + e->topic_len - 4, "/pwm", 4) == 0) {
            motor_apply(e->data, e->data_len);
        } else if (e->topic_len >= 7 && memcmp(e->topic + e->topic_len - 7, "/config", 7) == 0) {
            config_apply(e->data, e->data_len);
        } else {                                       /* remaining sub: reprovision */
            ESP_LOGW(TAG, "reprovision command — opening a provisioning window");
            s_provision_request = PROVISION_MAGIC;
            esp_restart();
        }
        break;
    default:
        break;
    }
}

static void operating_mode(char *ssid, const char *pass, const char *locator) {
    wifi_up();

    char discovered[33] = "";
    bool on_discovered = false;
    if (!ssid[0]) {
        if (!discover_hub(discovered)) {
            ESP_LOGW(TAG, "nothing stored and no open hub-* in range");
            goto fail;
        }
        ssid = discovered; pass = ""; on_discovered = true;
    }

    ESP_LOGI(TAG, "operating mode — joining '%s'", ssid);
    if (!wifi_join(ssid, pass)) {
        ESP_LOGE(TAG, "wifi join failed");
        /* Stored credentials can go stale (hub swapped, AP renamed) — a live
         * open hub-* beats a dead config. Discovery is never persisted: NVS
         * keeps explicit choices only, so the stored pair is retried first
         * on every boot. */
        if (!on_discovered && discover_hub(discovered)
            && strcmp(ssid, discovered) != 0 && wifi_join(discovered, "")) {
            ssid = discovered; on_discovered = true;
        } else {
            goto fail;
        }
    }

    /* Broker URI. Gateway-first (CONTRACT.md): on the hub's own AP the DHCP
     * gateway IS the hub, so <gateway>:1883 reaches the broker with no name
     * lookup and no hardcoded IP — the one address the two hub hosts (Pi,
     * ESP32) don't share. A stored locator overrides: a full mqtt:// URI is
     * used as-is, a bare host becomes mqtt://<host>:1883. */
    char uri[80];
    if (locator[0] && !on_discovered && strncmp(locator, "mqtt://", 7) == 0) {
        /* a valid stored MQTT locator (full URI) overrides discovery */
        snprintf(uri, sizeof uri, "%s", locator);
    } else {
        /* gateway IS the hub on its own AP (CONTRACT.md). Also the fallback when
         * the stored locator is a stale zenoh-era value (tcp/<ip>:7447) — not a
         * translatable MQTT URI, so ignore it rather than build a garbage host. */
        snprintf(uri, sizeof uri, "mqtt://" IPSTR ":1883", IP2STR(&s_gw));
    }
    ESP_LOGI(TAG, "broker %s as '%s'%s%s", uri, s_user,
             s_name[0] ? " — " : "", s_name);

    /* esp-mqtt authenticates with username/password — the capability
     * zenoh-pico lacked (usrpwd unimplemented), and the reason the rover ships
     * on MQTT. Same firmware reaches either hub: both are raw-TCP brokers on
     * :1883, and a rover never needs the WebSocket transport (that's the
     * browser's constraint). */
    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = uri,
        .credentials = {
            .username = s_user,
            .authentication.password = s_pass,
        },
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
    if (!cli) { ESP_LOGE(TAG, "mqtt init failed"); goto fail; }
    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    esp_mqtt_client_start(cli);

    /* First connect gates everything: it proves both reachability AND that the
     * team credential was accepted (a bad password disconnects here, never
     * reaching s_mqtt_up). No connect in 10 s → dead → provisioning window. */
    for (int i = 0; i < 40 && !s_mqtt_up; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_mqtt_up) { ESP_LOGE(TAG, "broker unreachable or credential rejected"); goto fail; }

    char key[48];
    snprintf(key, sizeof key, "robots/%s/sys", s_topic_id);
    ESP_LOGI(TAG, "publishing %s every 2 s", key);

    char buf[192];
    int down = 0;   /* consecutive 2 s ticks with no live session — dead → reprovision */
    for (;;) {
        if (s_mqtt_up) {
            down = 0;
            int64_t up_ms = esp_timer_get_time() / 1000;
            uint32_t heap = esp_get_free_heap_size();
            /* board id is metadata in the payload, not the topic (id == team). */
            snprintf(buf, sizeof buf,
                     "{\"uptime_ms\":%lld,\"free_heap\":%u,\"hw\":\"" HW_BOARD
                     "\",\"board\":\"%s\",\"synthetic\":false}",
                     (long long)up_ms, (unsigned)heap, s_id);
            if (esp_mqtt_client_publish(cli, key, buf, 0, 0, 0) >= 0)
                ESP_LOGI(TAG, "pub %s", buf);
            else
                ESP_LOGW(TAG, "publish enqueue failed");
        } else if (++down >= 10) {
            /* esp-mqtt auto-reconnects, so a brief hub outage self-heals; only
             * a sustained dead session (~20 s) drops to a provisioning window. */
            ESP_LOGE(TAG, "20 s with no broker — session dead");
            goto fail;
        } else {
            ESP_LOGW(TAG, "waiting for reconnect (%d/10)", down);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

fail:
    ESP_LOGW(TAG, "falling back to a provisioning window");
    s_provision_request = PROVISION_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ── provisioning mode: BLE (Improv Wi-Fi + hubcfg locator) ─────────────── */

static esp_timer_handle_t s_window;

static void window_expired(void *p) {
    if (provisioning_client_connected()) {
        /* Someone is mid-provisioning — give them another window. */
        esp_timer_start_once(s_window, PROVISION_WINDOW_US);
        return;
    }
    ESP_LOGI(TAG, "provisioning window expired — rebooting into operating mode");
    esp_restart();
}

/* Reboot-on-complete is debounced and deferred while a BLE client is attached:
 * s_done_cb fires in GATT write context, and an immediate restart there races
 * the peer's next operation (read-back, second write, Improv's PROVISIONED
 * notify) — the client sees "Device disconnected" mid-exchange. */
static esp_timer_handle_t s_done_reboot;
#define DONE_REBOOT_DELAY_US (4 * 1000000LL)

static void done_reboot(void *p) {
    if (provisioning_client_connected()) {
        esp_timer_start_once(s_done_reboot, DONE_REBOOT_DELAY_US);
        return;
    }
    ESP_LOGI(TAG, "provisioning complete — rebooting into operating mode");
    esp_restart();
}

static void on_provision_done(void) {
    if (!s_done_reboot) {
        const esp_timer_create_args_t t = {.callback = done_reboot, .name = "prov_done"};
        ESP_ERROR_CHECK(esp_timer_create(&t, &s_done_reboot));
    }
    esp_timer_stop(s_done_reboot);   /* no-op if not armed */
    ESP_ERROR_CHECK(esp_timer_start_once(s_done_reboot, DONE_REBOOT_DELAY_US));
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    provisioning_advertise(s_id);
}

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void provisioning_mode(void) {
    ESP_LOGI(TAG, "provisioning mode — advertising %s", s_id);
    led_set(true);
    /* Always bounded: expiry reboots into operating mode, which retries the
     * stored credentials or re-scans for a hub. A rover powered on before its
     * hub alternates scan → window until the hub appears — no human needed. */
    const esp_timer_create_args_t t = {.callback = window_expired, .name = "prov_window"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_window));
    ESP_ERROR_CHECK(esp_timer_start_once(s_window, PROVISION_WINDOW_US));
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(s_id);
    provisioning_register();
    provisioning_set_done_cb(on_provision_done);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    /* FreeRTOS tasks now running; app_main returns and the idle task takes over */
}

/* ── app_main: dispatch to exactly one radio path ────────────────────────── */

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    rover_format_robot_id(mac, s_id);
    ESP_LOGI(TAG, "robot id: %s", s_id);

    /* A team assigned post-join (NVS) overrides the compile-time MQTT identity. */
    char cu[33], cp[65], cn[33];
    rover_config_load_identity(cu, cp, cn);
    if (cu[0]) {
        strncpy(s_user, cu, sizeof s_user - 1);
        strncpy(s_pass, cp, sizeof s_pass - 1);
        strncpy(s_name, cn, sizeof s_name - 1);
        ESP_LOGI(TAG, "identity from NVS: %s%s%s", s_user, s_name[0] ? " / " : "", s_name);
    }

    bool provision_requested = (s_provision_request == PROVISION_MAGIC);
    s_provision_request = 0;

    led_set(false);   /* also pins the active-low pin high so it can't glow half-lit */
    xTaskCreate(button_task, "button", 2048, NULL, 5, NULL);

    char ssid[33], pass[65], loc[65];
    rover_config_load(ssid, pass, loc);

    if (!provision_requested) {
        s_operating = true;
        operating_mode(ssid, pass, loc);   /* never returns */
    }
    provisioning_mode();
}
