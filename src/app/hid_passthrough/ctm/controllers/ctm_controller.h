#ifndef CTM_CONTROLLER_H
#define CTM_CONTROLLER_H

/* Controller abstraction (D2). One mechanism per detected controller: each
 * type supplies an ops vtable; the factory picks the right ops for a device.
 *
 * STAGE 1 (scaffold, this commit): the interface + classification (matches) +
 * the factory. The shared byte pump in controller_common.c and the wiring into
 * plug_in_item are STAGE 2 — until then these ops do not yet drive live
 * sessions; the proven tv_bridge_worker / ctm_hidraw_bridge paths still run.
 *
 * The layer is UI-independent: the app fills a neutral ctm_controller_dev_t
 * from its logical_device_t, so controllers/ does not depend on app/ types. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_transport.h"
#include "ctm_settings.h"       /* tv_bridge_worker_settings_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char vid[8];
    char pid[8];
    char bus[8];      /* "USB" / "BT" */
    char name[128];
    char path[64];    /* /dev/hidrawN */
    char mac[64];
} ctm_controller_dev_t;

typedef struct ctm_controller ctm_controller_t;   /* opaque; defined in stage 2 */

typedef struct {
    const char *kind;   /* "ds5" / "ds4" / "xbox" / "steam_puck" / "generic" */

    /* Behaviour flags preserving each path's proven semantics in the shared
     * pump. DS (worker) = all true; puck/xbox/generic (relay) = all false. */
    bool needs_host_config;   /* block for HOST_CONFIG after HELLO (DS pacing) */
    bool grab_evdev;          /* EVIOCGRAB the device's evdev nodes (BT/DS) */
    bool request_bt_mode;     /* send the Sony feature-0x05 full-BT-mode probe */
    bool composite;           /* forward EVERY HID interface, each tagged by its IN
                               * endpoint (puck); host plugs the whole composite. */
    bool raw_acl_output;      /* inject high-rate 0x36 output via raw HCI-ACL,
                               * bypassing the webOS one-outstanding flow control
                               * (DS5 only); falls back to hidraw on any failure. */

    /* Does this type claim the device? Factory tries specific types first,
     * generic last. */
    bool (*matches)(const ctm_controller_dev_t *dev);

    /* Choose which /dev/hidrawN to bridge for composite devices. NULL => the
     * app default (dev->path). steam_puck overrides to pick the gamepad
     * interface (fixes the §6a "first hidraw wins" bug). */
    int (*select_node)(const ctm_controller_dev_t *dev, char *out, size_t out_len);

    /* Optional one-shot init after plug, before the pump starts (xbox GIP
     * handshake, puck lizard-mode exit). NULL => none. */
    int (*on_plug_init)(ctm_controller_t *c, ctm_transport_t *t);

    /* Patch an outbound report in place before it reaches the device (DS audio
     * route / volume / CRC). Returns nonzero to DROP the report (suppress the
     * write), 0 to write `*len` bytes. NULL => verbatim forward. */
    int (*patch_output)(ctm_controller_t *c, uint8_t *buf, size_t *len);

    /* Live UI settings update (DS sliders). NULL => ignored. */
    void (*set_settings)(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);
} ctm_controller_ops_t;

/* Live bridging status — read-only snapshot for the UI status panel. */
typedef struct {
    bool connected;              /* a session is up (handshake done, looping) */
    bool transport_enet;         /* true = ENet/UDP, false = TCP */
    unsigned long reports_in;    /* input reports forwarded to the host */
    unsigned long reports_out;   /* output reports written to the device */
    char last_event[96];         /* most recent controller log line */
} ctm_controller_status_t;

/* --- lifecycle (controller_common.c) ----------------------------------------
 * Each controller runs in isolation: its own pump (reader + session threads),
 * HID fd, transport, settings, and per-MAC log file. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev);
int  ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port);
void ctm_controller_plug_out(ctm_controller_t *c);
/* Composite: forwarded enumeration payload (CTMB_MSG_ENUM), sent before HELLO. */
void ctm_controller_set_enum_payload(ctm_controller_t *c, const uint8_t *payload, int len);
void ctm_controller_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s);
void ctm_controller_get_settings(ctm_controller_t *c, tv_bridge_worker_settings_t *out);
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out);
void ctm_controller_destroy(ctm_controller_t *c);

/* Register a sink for controller log lines (e.g. the app's on-screen console).
 * NULL = file + stderr only. The sink must be thread-safe — controllers log
 * from their own threads. When: app startup. */
void ctm_controller_set_log_sink(void (*sink)(const char *line));

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) to the trailing 4
 * bytes. When: a DS patch_output hook after rewriting a report. Defined in
 * controller_common.c so DS4 + DS5 share it. */
void ctm_bt_sign_output(uint8_t *data, size_t len);

/* Per-type ops tables, defined in controller_<kind>.c. */
extern const ctm_controller_ops_t ctm_controller_ds5_ops;
extern const ctm_controller_ops_t ctm_controller_ds4_ops;
extern const ctm_controller_ops_t ctm_controller_xbox_ops;
extern const ctm_controller_ops_t ctm_controller_steam_puck_ops;
extern const ctm_controller_ops_t ctm_controller_generic_ops;

/* Pick ops for a device: specific types first (puck/ds5/ds4/xbox), generic
 * fallback. Never returns NULL. */
const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* CTM_CONTROLLER_H */
