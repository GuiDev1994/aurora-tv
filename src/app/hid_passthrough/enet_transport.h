#ifndef CTM_ENET_TRANSPORT_H
#define CTM_ENET_TRANSPORT_H

#include "ctm_bridge_protocol.h"

#include <stddef.h>
#include <stdint.h>

int enet_client_global_init(void);

void enet_client_global_deinit(void);

typedef struct ctm_enet_client ctm_enet_client_t;

ctm_enet_client_t *enet_client_create(void);

void enet_client_destroy(ctm_enet_client_t *client);

int enet_client_connect(ctm_enet_client_t *client, const char *host, int port, unsigned int timeout_ms);

void enet_client_disconnect(ctm_enet_client_t *client);

int enet_client_connected(const ctm_enet_client_t *client);

int enet_client_service(ctm_enet_client_t *client, unsigned int timeout_ms);

int enet_client_send_msg(ctm_enet_client_t *client, uint16_t type, uint32_t flags, uint32_t request_id,
                         const void *payload, size_t len);

int enet_client_recv_msg(ctm_enet_client_t *client, ctmb_header_t *h, uint8_t **payload);

#endif
