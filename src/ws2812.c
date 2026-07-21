/*
 * ws2812.c — single-pixel WS2812 LED for boards whose only onboard LED is an
 * addressable RGB (the Freenove ESP32-S3 CAM, GPIO48), driven through the
 * maintained espressif/led_strip component (RMT backend, pinned in
 * idf_component.yml, target-gated to esp32s3). robot_role.c's led_set() calls
 * ws2812_set_rgb() so the connected-indicator and cmd/identify blink behave
 * exactly as on a plain-GPIO board; the classic boards never compile this file nor
 * pull led_strip (both gated on LED_WS2812_GPIO / target == esp32s3).
 *
 * A hand-rolled RMT bytes-encoder came first and stalled on the S3 — the transmit
 * never signalled done (rmt_tx_wait_all_done flush timeout). led_strip is the
 * reference WS2812 encoding the examples ship, so this defers the RMT timing/refill
 * subtleties to it rather than re-debugging them here.
 */
#include "roles.h"

#ifdef LED_WS2812_GPIO
#include "led_strip.h"

static led_strip_handle_t s_strip;

void ws2812_init(int gpio)
{
    if (s_strip) return;   /* idempotent — led_set() lazy-inits on first use */
    led_strip_config_t strip = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 0.1 us tick — ample for WS2812 bit timing */
        .mem_block_symbols = 64,
    };
    if (led_strip_new_rmt_device(&strip, &rmt, &s_strip) != ESP_OK) s_strip = NULL;
}

void ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);   /* wire order handled by color_component_format */
    led_strip_refresh(s_strip);
}
#endif
