#ifndef ROVER_PROVISIONING_H
#define ROVER_PROVISIONING_H

#include <stdbool.h>
#include "host/ble_uuid.h"

// Canonical hubcfg UUIDs — MUST match better-robotics/provision/rover.html.
// Generated 2026-06-26.
#define HUBCFG_SERVICE_UUID "dd001f17-5e75-4dfc-b611-70ef1e6bb9ca"
#define HUBCFG_LOCATOR_UUID "4941adfa-0a40-460f-9096-39d1db36f53b"

/* Register the hubcfg + Improv GATT services (one combined table). */
void provisioning_register(void);

typedef void (*provisioning_done_cb)(void);
void provisioning_set_done_cb(provisioning_done_cb cb);

/* True while a BLE central is connected (lets a provisioning window extend
 * rather than cut off a session in progress). */
bool provisioning_client_connected(void);

/* Start / restart BLE advertising with Improv UUID + local name.
 * Owns the GAP event callback internally; safe to call from on_sync and
 * the disconnect/reconnect path.  Caches 'name' so the internal GAP
 * callback can re-advertise after a disconnect without a second call
 * from main. */
void provisioning_advertise(const char *name);

#endif // ROVER_PROVISIONING_H
