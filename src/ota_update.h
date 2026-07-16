#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

/* Wi-Fi firmware push for the unified image (POST /ota), so updating a
 * classroom fleet stops meaning "find every board and plug it in".
 *
 * Registers onto the portal's shared :80 (wifi_portal_httpd), the same way
 * ws_mqtt_bridge.c registers the dashboard — no new listener, no new port, and
 * both boot roles get it from one call site each.
 *
 *   POST /ota   body = firmware.bin, auth = HTTP Basic as "instructor".
 *               Streams to the inactive slot, then reboots into it.
 *
 * Safety is the bootloader's, not ours: partitions.csv is ota_0 + ota_1 and
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE makes a freshly-written slot boot as
 * PENDING_VERIFY. This module's mark-valid timer is what confirms the new image
 * actually came up; an image that panics, hangs the WDT or loses power before
 * then is reverted by the bootloader on the next boot, with no USB trip.
 *
 * What that does NOT cover: an image that boots cleanly and is broken
 * afterwards. Rollback tests "did it come up", never "is it correct".
 *
 * Call once per boot, AFTER wifi_portal_start() (it needs the shared handle)
 * and after the AP is up. */
void ota_update_start(void);

#endif
