/* The board's ESP_LOG ring, served at GET /log. See device_log.h. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdbool.h>
#include "esp_timer.h"

#include "device_log.h"
#include "roles.h"
#include "wifi_portal.h"

/* 6 KB of the chip's 8 KB RTC slow memory — everything below lives in that 8 KB
 * and must fit WITH the counters, which is what rules out a round 8192. Nothing
 * else in this firmware touches RTC memory and the ULP is disabled, so this is
 * otherwise-idle space.
 * Sized against a measurement, not a guess: one boot of this firmware logs more
 * than 4 KB, so at 4 KB a board that crashed mid-boot showed only its dying
 * moments with no run-up. 6 KB is still not a whole boot — the ring is a
 * scrollback, not an archive, and a crash makes the NEWEST lines the interesting
 * ones, which is the end a ring keeps. */
#define LOG_RING   6144
#define LOG_MAGIC  0x6C6F6732   /* "log2" — bumped: s_pos/s_wrapped replaced a
                                 * free-running counter, so a ring surviving a
                                 * reboot from the old layout must be discarded,
                                 * not reinterpreted. */

/* RTC_NOINIT, not RTC_DATA: RTC_DATA is zeroed on a normal boot, which would
 * erase the crash we rebooted to investigate. NOINIT keeps it, and the magic
 * above is what tells kept-across-a-crash apart from never-initialised.
 *
 * A cursor + a flag, NOT a free-running byte total. The total was neater to read
 * but forced LOG_RING to divide 2^32 or the modulo silently mis-slices the ring
 * once the counter wraps — which meant the size had to be 4096 or 8192, and 8192
 * does not fit beside these counters in 8 KB of RTC. The design constrained the
 * size for no benefit; this one works at any size. */
RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint32_t s_pos;            /* write cursor, 0..LOG_RING-1 */
RTC_NOINIT_ATTR static uint32_t s_wrapped;        /* has the cursor lapped once? */
RTC_NOINIT_ATTR static uint32_t s_boot;           /* boot counter, for the banner */
RTC_NOINIT_ATTR static char     s_ring[LOG_RING];

/* The reset reason is a FIELD, not a log line, and that distinction was learned
 * on the bench (2026-07-16): a single boot logs more than the ring holds, so the
 * boot banner — written first — is evicted first. The one fact worth crossing a
 * reboot for was reliably the one fact the ring threw away soonest. /log renders
 * this into a preamble at serve time, where nothing can push it out. */
RTC_NOINIT_ATTR static uint32_t s_reset;

/* A spinlock and a static line buffer, NOT a mutex and a stack buffer.
 *   - static: this hook runs on whatever task logged, and some of them have
 *     2 KB stacks (the reboot tasks). A 192-byte frame added to every log call
 *     is a stack overflow looking for the smallest task in the system.
 *   - spinlock: esp_log_write is reachable from contexts where taking a mutex
 *     is fatal. The critical section covers one vsnprintf and a byte copy. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* 256: the rover's own telemetry line ("pub {…}", every 2 s) is ~250 bytes and
 * is by far the most common thing in this ring. It is static, so its size costs
 * DRAM once and nothing per call — 192 was chosen against a stack cost this
 * design does not pay. */
static char s_line[256];
static vprintf_like_t s_chain;   /* the vprintf we replaced — serial still works */

static void ring_put(const char *p, int n)
{
    for (int i = 0; i < n; i++) {
        s_ring[s_pos++] = p[i];
        if (s_pos >= LOG_RING) { s_pos = 0; s_wrapped = 1; }
    }
}

static int log_vprintf(const char *fmt, va_list ap)
{
    /* ap is consumed by whoever reads it first, so the ring and the UART each
     * need their own copy — using it twice is undefined, not merely wrong. */
    va_list ap2;
    va_copy(ap2, ap);

    portENTER_CRITICAL(&s_mux);
    /* vsnprintf reports the length it WANTED, not what it wrote. */
    int n = vsnprintf(s_line, sizeof s_line, fmt, ap2);
    if (n > 0) {
        int w = n < (int)sizeof s_line ? n : (int)sizeof s_line - 1;
        ring_put(s_line, w);
        /* A truncated line loses its own trailing "\n" — that newline is the
         * last thing in the format string, so it is the first casualty. The ring
         * then holds one enormous line instead of a log: unreadable in a browser,
         * un-greppable everywhere, and it degrades only once the ring fills with
         * the longest lines, which is to say only in the classroom and never on
         * the bench. Bench-caught 2026-07-16: a 6 KB ring came back with exactly
         * ONE newline in it. Guarantee the line ending rather than size a buffer
         * and hope. */
        if (s_line[w - 1] != '\n') ring_put("…\n", 4);   /* … marks the cut */
    }
    portEXIT_CRITICAL(&s_mux);
    va_end(ap2);

    /* Serial last and OUTSIDE the lock: this is a 115200 UART, and holding a
     * spinlock across it would block every other core and task for milliseconds
     * per line. The ring is the fast path; the cable is the slow one. */
    return s_chain ? s_chain(fmt, ap) : vprintf(fmt, ap);
}

static const char *reset_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

void device_log_init(void)
{
    esp_reset_reason_t rr = esp_reset_reason();

    /* Power-on means RTC memory holds whatever the SRAM cells felt like; the
     * magic would catch that most of the time, and "most of the time" is not a
     * property to hand a classroom. Clear on both signals. */
    if (s_magic != LOG_MAGIC || rr == ESP_RST_POWERON) {
        s_magic = LOG_MAGIC;
        s_pos = 0;
        s_wrapped = 0;
        s_boot = 0;
        memset(s_ring, 0, sizeof s_ring);
    }
    /* Belt and braces: RTC memory that passed the magic can still hold a cursor
     * from a half-written moment. An out-of-range s_pos would index past the
     * ring on the very first log line of the boot after a crash. */
    if (s_pos >= LOG_RING) { s_pos = 0; s_wrapped = 1; }
    s_boot++;
    s_reset = (uint32_t)rr;

    s_chain = esp_log_set_vprintf(log_vprintf);

    /* The banner is written INTO the ring, so boot boundaries are visible in
     * the log itself rather than being something the reader has to infer. On
     * the boot after a crash this line is the diagnosis: it names the reset
     * reason directly under the last thing the dying image managed to say. */
    ESP_LOGW("boot", "──── boot #%u ── reset: %s ────", (unsigned)s_boot, reset_str(rr));
}

static esp_err_t log_get(httpd_req_t *req)
{
    /* Snapshot onto the heap, not the stack: the httpd task's stack is 4 KB and
     * this buffer alone is bigger. Freed before returning; ~170 KB free at rest. */
    char *out = malloc(LOG_RING);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");

    /* Not yet lapped → the ring is a plain buffer, 0..s_pos, and everything ever
     * logged is here. Lapped → oldest byte is the one under the cursor. */
    portENTER_CRITICAL(&s_mux);
    bool wrapped = s_wrapped != 0;
    size_t n = wrapped ? LOG_RING : s_pos;
    size_t start = wrapped ? s_pos : 0;
    for (size_t i = 0; i < n; i++) out[i] = s_ring[(start + i) % LOG_RING];
    portEXIT_CRITICAL(&s_mux);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    /* Same CORS stance as /fleet: the dashboard is served by the hub and reads
     * this from a different origin (the board's own IP), so without this header
     * the browser discards a 200 the board happily sent. A GET with no custom
     * headers is a "simple" request, so this alone is enough — no preflight. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* The preamble carries what the ring cannot keep — see s_reset. Sent first,
     * as its own chunk, so the verdict arrives before the scrollback and a
     * wrapped ring (every boot wraps it) can never bury it. */
    char pre[160];
    int pn = snprintf(pre, sizeof pre,
                      "==== boot #%u · reset: %s · up %llus · %d B ring%s ====\n",
                      (unsigned)s_boot, reset_str((esp_reset_reason_t)s_reset),
                      (unsigned long long)(esp_timer_get_time() / 1000000LL), LOG_RING,
                      wrapped ? " (wrapped, oldest lines dropped)" : "");
    httpd_resp_send_chunk(req, pre, pn);
    httpd_resp_send_chunk(req, out, n);
    free(out);
    return httpd_resp_send_chunk(req, NULL, 0);   /* NULL/0 terminates the chunked body */
}

void device_log_serve(void)
{
    httpd_handle_t s = wifi_portal_httpd();
    if (!s) {
        ESP_LOGE("log", "no shared :80 — /log not registered (call after wifi_portal_start)");
        return;
    }
    httpd_uri_t u = { .uri = "/log", .method = HTTP_GET, .handler = log_get };
    /* Counted against wifi_portal.c's max_uri_handlers — see its comment. */
    httpd_register_uri_handler(s, &u);
    ESP_LOGI("log", "device log at GET /log (%d B ring, survives a reboot)", LOG_RING);
}
