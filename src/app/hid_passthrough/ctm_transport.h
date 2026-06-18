#ifndef CTM_TRANSPORT_H
#define CTM_TRANSPORT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "ctm_bridge_protocol.h"
#include "enet_transport.h"

typedef enum {
    CTM_TRANSPORT_NONE = 0,
    CTM_TRANSPORT_TCP = 1,
    CTM_TRANSPORT_ENET = 2
} ctm_transport_kind_t;

typedef struct {
    ctm_transport_kind_t kind;
    int fd;
    ctm_enet_client_t *enet;
    pthread_mutex_t send_mutex;
    uint32_t send_sequence;
} ctm_transport_t;

void ctm_transport_init(ctm_transport_t *t, ctm_enet_client_t *enet);

void ctm_transport_destroy(ctm_transport_t *t);

int ctm_transport_connect_once(ctm_transport_t *t, const char *host, int port, unsigned int enet_timeout_ms);

int ctm_transport_send_msg(ctm_transport_t *t, uint16_t type, uint32_t flags, uint32_t request_id,
                           const void *payload, size_t len);

int ctm_transport_recv_msg(ctm_transport_t *t, ctmb_header_t *h, uint8_t **payload);

int ctm_transport_service(ctm_transport_t *t, unsigned int timeout_ms);

int ctm_transport_connected(const ctm_transport_t *t);

void ctm_transport_disconnect(ctm_transport_t *t);

#endif
