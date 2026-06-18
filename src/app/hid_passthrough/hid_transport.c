#include "hid_transport.h"

#include "ctm_transport.h"
#include "enet_transport.h"

#include <errno.h>
#include <string.h>

static int ctmb_connect(void *ctx, const char *host, int port) {
    ctmb_hid_transport_ctx_t *c = (ctmb_hid_transport_ctx_t *) ctx;
    return ctm_transport_connect_once(&c->transport, host, port, 400);
}

static int ctmb_send(void *ctx, uint16_t type, uint32_t flags, uint32_t request_id, const void *payload, size_t len) {
    ctmb_hid_transport_ctx_t *c = (ctmb_hid_transport_ctx_t *) ctx;
    return ctm_transport_send_msg(&c->transport, type, flags, request_id, payload, len);
}

static int ctmb_recv(void *ctx, ctmb_header_t *h, uint8_t **payload) {
    ctmb_hid_transport_ctx_t *c = (ctmb_hid_transport_ctx_t *) ctx;
    return ctm_transport_recv_msg(&c->transport, h, payload);
}

static int ctmb_service(void *ctx, unsigned int timeout_ms) {
    ctmb_hid_transport_ctx_t *c = (ctmb_hid_transport_ctx_t *) ctx;
    return ctm_transport_service(&c->transport, timeout_ms);
}

static void ctmb_disconnect(void *ctx) {
    ctmb_hid_transport_ctx_t *c = (ctmb_hid_transport_ctx_t *) ctx;
    ctm_transport_disconnect(&c->transport);
}

static int ctmb_connected(const void *ctx) {
    const ctmb_hid_transport_ctx_t *c = (const ctmb_hid_transport_ctx_t *) ctx;
    return ctm_transport_connected(&c->transport);
}

static const hid_transport_vtable_t ctmb_vtable = {
        .connect = ctmb_connect,
        .send_msg = ctmb_send,
        .recv_msg = ctmb_recv,
        .service = ctmb_service,
        .disconnect = ctmb_disconnect,
        .connected = ctmb_connected,
};

void ctmb_hid_transport_init(hid_transport_t *out, ctmb_hid_transport_ctx_t *ctx_storage) {
    ctmb_hid_transport_ctx_t *c = ctx_storage;
    memset(c, 0, sizeof(*c));
    c->enet = enet_client_create();
    c->enet_owned = c->enet != NULL ? 1 : 0;
    ctm_transport_init(&c->transport, c->enet);
    out->vtable = &ctmb_vtable;
    out->ctx = c;
}

void ctmb_hid_transport_destroy(ctmb_hid_transport_ctx_t *ctx_storage) {
    ctmb_hid_transport_ctx_t *c = ctx_storage;
    ctm_transport_disconnect(&c->transport);
    ctm_transport_destroy(&c->transport);
    if (c->enet_owned && c->enet) {
        enet_client_destroy(c->enet);
    }
    c->enet = NULL;
}

/* WebSocket transport — reserved for a future gateway; CTM-USBIP uses CTMB over TCP/ENet. */

typedef struct {
    int unused;
} ws_hid_transport_ctx_t;

static int ws_connect(void *ctx, const char *host, int port) {
    (void) ctx;
    (void) host;
    (void) port;
    errno = ENOTSUP;
    return -1;
}

static int ws_send(void *ctx, uint16_t type, uint32_t flags, uint32_t request_id, const void *payload, size_t len) {
    (void) ctx;
    (void) type;
    (void) flags;
    (void) request_id;
    (void) payload;
    (void) len;
    errno = ENOTSUP;
    return -1;
}

static int ws_recv(void *ctx, ctmb_header_t *h, uint8_t **payload) {
    (void) ctx;
    (void) h;
    (void) payload;
    return -1;
}

static int ws_service(void *ctx, unsigned int timeout_ms) {
    (void) ctx;
    (void) timeout_ms;
    return 0;
}

static void ws_disconnect(void *ctx) {
    (void) ctx;
}

static int ws_connected(const void *ctx) {
    (void) ctx;
    return 0;
}

static const hid_transport_vtable_t ws_vtable = {
        .connect = ws_connect,
        .send_msg = ws_send,
        .recv_msg = ws_recv,
        .service = ws_service,
        .disconnect = ws_disconnect,
        .connected = ws_connected,
};

void ws_hid_transport_init(hid_transport_t *out, void *ctx_storage) {
    ws_hid_transport_ctx_t *c = (ws_hid_transport_ctx_t *) ctx_storage;
    memset(c, 0, sizeof(*c));
    out->vtable = &ws_vtable;
    out->ctx = c;
}
