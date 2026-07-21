#ifndef DEVICE_LOG_H
#define DEVICE_LOG_H

/* The board's own ESP_LOG output, readable over HTTP — "why did my robot do
 * that" without a serial cable, which in a classroom means without a laptop,
 * a USB lead, and knowing what a serial monitor is.
 *
 *   GET /log   text/plain, oldest line first, CORS-open like /fleet.
 *
 * The ring lives in RTC_NOINIT memory, which is the entire point: it SURVIVES
 * a panic, a watchdog reset and a reboot. A log you can only read while the
 * board is healthy cannot answer the one question worth asking — a crashed
 * board's serial output is exactly what nobody was watching. Each boot writes
 * its own banner into the ring (with esp_reset_reason), so the reader sees the
 * lines leading up to a crash and the crash itself, in order, across the
 * reboot that would otherwise have erased them.
 *
 * What it CANNOT show: anything logged before device_log_init runs (the
 * bootloader's own ROM output is on the UART only), and the panic backtrace
 * itself — the panic handler writes straight to the UART without going through
 * esp_log, so the ring shows what led up to the crash and then the next boot's
 * banner naming the reset reason. That reason is the diagnosis; the backtrace
 * still needs a cable.
 *
 * device_log_init  — install the log hook. Call FIRST from app_main, before any
 *                    role: everything logged before it is lost, and boot is
 *                    when the interesting failures happen.
 * device_log_serve — register GET /log. Call after wifi_portal_start, next to
 *                    ota_update_start (needs the shared :80 handle). */
void device_log_init(void);
void device_log_serve(void);

#endif
