#pragma once

/**
 * Wakes up Steam Controllers paired over Bluetooth LE (issue #81).
 *
 * webOS never does an ATT MTU exchange with the controller, so the MTU stays at 23 and the
 * controller sends no input at all: it pairs and shows as connected, but even the trackpad
 * mouse stays silent. A background thread watches for a connected Steam Controller and
 * asks com.webos.service.blegatt for a larger MTU (client/connect, then client/requestMtu
 * once the connect subscription reports the link up), again after every reconnect. The calls
 * go through luna-send-pub, since apps in developer mode may not call requestMtu themselves.
 * A controller another app already has a GATT client for is left alone. No root needed.
 *
 * The client/connect subscription has to stay open: ending it ends the GATT client. It stays
 * open while the controller is connected, and client/disconnect is called
 * when it goes away or the app exits: without it, the next client/connect for that address
 * fails with "SubscriptionInfo is already existed" until the TV reboots.
 */
typedef struct webos_steam_ble_state webos_steam_ble_state_t;

webos_steam_ble_state_t *webos_steam_ble_start(void);

/** Disconnects the GATT clients it opened. Waits at most about a second for the thread. */
void webos_steam_ble_stop(webos_steam_ble_state_t *state);
