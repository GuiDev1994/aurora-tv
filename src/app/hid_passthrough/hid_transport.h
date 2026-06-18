#ifndef HID_TRANSPORT_H
#define HID_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "ctm_bridge_protocol.h"
#include "ctm_transport.h"
#include "enet_transport.h"

typedef struct ctmb_hid_transport_ctx {
    ctm_transport_t transport;
    ctm_enet_client_t *enet;
    int enet_owned;
} ctmb_hid_transport_ctx_t;

typedef struct hid_transport_vtable {
    int (*connect)(void *ctx, const char *host, int port);
    int (*send_msg)(void *ctx, uint16_t type, uint32_t flags, uint32_t request_id, const void *payload, size_t len);
    int (*recv_msg)(void *ctx, ctmb_header_t *h, uint8_t **payload);
    int (*service)(void *ctx, unsigned int timeout_ms);
    void (*disconnect)(void *ctx);
    int (*connected)(const void *ctx);
} hid_transport_vtable_t;

typedef struct hid_transport {
    const hid_transport_vtable_t *vtable;
    void *ctx;
} hid_transport_t;

static inline int hid_transport_connect(hid_transport_t *t, const char *host, int port) {
    return t && t->vtable && t->vtable->connect ? t->vtable->connect(t->ctx, host, port) : -1;
}

static inline int hid_transport_send_msg(hid_transport_t *t, uint16_t type, uint32_t flags, uint32_t request_id,
                                         const void *payload, size_t len) {
    return t && t->vtable && t->vtable->send_msg ? t->vtable->send_msg(t->ctx, type, flags, request_id, payload, len)
                                                : -1;
}

static inline int hid_transport_recv_msg(hid_transport_t *t, ctmb_header_t *h, uint8_t **payload) {
    return t && t->vtable && t->vtable->recv_msg ? t->vtable->recv_msg(t->ctx, h, payload) : -1;
}

static inline int hid_transport_service(hid_transport_t *t, unsigned int timeout_ms) {
    return t && t->vtable && t->vtable->service ? t->vtable->service(t->ctx, timeout_ms) : 0;
}

static inline void hid_transport_disconnect(hid_transport_t *t) {
    if (t && t->vtable && t->vtable->disconnect) {
        t->vtable->disconnect(t->ctx);
    }
}

static inline int hid_transport_connected(const hid_transport_t *t) {
    return t && t->vtable && t->vtable->connected ? t->vtable->connected(t->ctx) : 0;
}

/* CTMB transport (TCP + ENet dual-probe). */
void ctmb_hid_transport_init(hid_transport_t *out, ctmb_hid_transport_ctx_t *ctx_storage);

void ctmb_hid_transport_destroy(ctmb_hid_transport_ctx_t *ctx_storage);

/* WebSocket stub — CTM-USBIP does not speak WebSocket today. */
void ws_hid_transport_init(hid_transport_t *out, void *ctx_storage);

#endif
