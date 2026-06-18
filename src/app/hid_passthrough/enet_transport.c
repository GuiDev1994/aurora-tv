#include "enet_transport.h"

#include <enet/enet.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTM_ENET_CHANNEL 0
#define CTM_ENET_CHANNEL_COUNT 1
#define CTM_ENET_INBOX_CAP 256
#define CTM_ENET_OUTBOX_CAP 256

typedef struct {
    ctmb_header_t header;
    uint8_t *payload;
} ctm_enet_msg_t;

struct ctm_enet_client {
    ENetHost *host;
    ENetPeer *peer;
    int connected;
    uint32_t send_sequence;

    pthread_mutex_t out_mutex;
    ctm_enet_msg_t outbox[CTM_ENET_OUTBOX_CAP];
    int out_head;
    int out_count;

    pthread_mutex_t in_mutex;
    ctm_enet_msg_t inbox[CTM_ENET_INBOX_CAP];
    int in_head;
    int in_count;
};

static uint64_t enet_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

int enet_client_global_init(void) {
    return enet_initialize();
}

void enet_client_global_deinit(void) {
    enet_deinitialize();
}

ctm_enet_client_t *enet_client_create(void) {
    ctm_enet_client_t *client = (ctm_enet_client_t *) calloc(1, sizeof(*client));
    if (!client) {
        return NULL;
    }
    client->host = enet_host_create(AF_INET, NULL, 1, CTM_ENET_CHANNEL_COUNT, 0, 0);
    if (!client->host) {
        free(client);
        return NULL;
    }
    pthread_mutex_init(&client->out_mutex, NULL);
    pthread_mutex_init(&client->in_mutex, NULL);
    return client;
}

static void free_msg_ring(ctm_enet_msg_t *ring, int cap, int head, int count) {
    for (int i = 0; i < count; i++) {
        int idx = (head + i) % cap;
        free(ring[idx].payload);
        ring[idx].payload = NULL;
    }
}

static void enet_client_reset_queues(ctm_enet_client_t *client) {
    pthread_mutex_lock(&client->out_mutex);
    free_msg_ring(client->outbox, CTM_ENET_OUTBOX_CAP, client->out_head, client->out_count);
    client->out_head = 0;
    client->out_count = 0;
    pthread_mutex_unlock(&client->out_mutex);

    pthread_mutex_lock(&client->in_mutex);
    free_msg_ring(client->inbox, CTM_ENET_INBOX_CAP, client->in_head, client->in_count);
    client->in_head = 0;
    client->in_count = 0;
    pthread_mutex_unlock(&client->in_mutex);
}

void enet_client_destroy(ctm_enet_client_t *client) {
    if (!client) {
        return;
    }
    if (client->peer) {
        enet_peer_reset(client->peer);
        client->peer = NULL;
    }
    enet_client_reset_queues(client);
    if (client->host) {
        enet_host_destroy(client->host);
        client->host = NULL;
    }
    pthread_mutex_destroy(&client->out_mutex);
    pthread_mutex_destroy(&client->in_mutex);
    free(client);
}

static void enet_client_ingest(ctm_enet_client_t *client, const enet_uint8 *data, size_t length) {
    if (!data || length < sizeof(ctmb_header_t)) {
        return;
    }
    ctmb_header_t header;
    memcpy(&header, data, sizeof(header));
    if (header.magic != CTMB_MAGIC || header.version != CTMB_VERSION || header.payload_len > CTMB_MAX_PAYLOAD) {
        return;
    }
    if (length < sizeof(ctmb_header_t) + header.payload_len) {
        return;
    }

    uint8_t *payload = NULL;
    if (header.payload_len) {
        payload = (uint8_t *) malloc(header.payload_len);
        if (!payload) {
            return;
        }
        memcpy(payload, data + sizeof(ctmb_header_t), header.payload_len);
    }

    pthread_mutex_lock(&client->in_mutex);
    if (client->in_count >= CTM_ENET_INBOX_CAP) {
        free(client->inbox[client->in_head].payload);
        client->inbox[client->in_head].payload = NULL;
        client->in_head = (client->in_head + 1) % CTM_ENET_INBOX_CAP;
        client->in_count--;
    }
    int idx = (client->in_head + client->in_count) % CTM_ENET_INBOX_CAP;
    client->inbox[idx].header = header;
    client->inbox[idx].payload = payload;
    client->in_count++;
    pthread_mutex_unlock(&client->in_mutex);
}

static void enet_client_dispatch(ctm_enet_client_t *client, const ctm_enet_msg_t *msg) {
    if (!client->peer) {
        return;
    }
    size_t total = sizeof(ctmb_header_t) + msg->header.payload_len;
    enet_uint32 packet_flags =
            (msg->header.type == CTMB_MSG_INPUT_REPORT) ? 0u : ENET_PACKET_FLAG_RELIABLE;
    ENetPacket *packet = enet_packet_create(NULL, total, packet_flags);
    if (!packet) {
        return;
    }
    memcpy(packet->data, &msg->header, sizeof(ctmb_header_t));
    if (msg->header.payload_len && msg->payload) {
        memcpy(packet->data + sizeof(ctmb_header_t), msg->payload, msg->header.payload_len);
    }
    if (enet_peer_send(client->peer, CTM_ENET_CHANNEL, packet) < 0) {
        enet_packet_destroy(packet);
    }
}

static void enet_client_flush_outbox(ctm_enet_client_t *client) {
    for (;;) {
        ctm_enet_msg_t msg;
        pthread_mutex_lock(&client->out_mutex);
        if (client->out_count <= 0) {
            pthread_mutex_unlock(&client->out_mutex);
            break;
        }
        msg = client->outbox[client->out_head];
        client->outbox[client->out_head].payload = NULL;
        client->out_head = (client->out_head + 1) % CTM_ENET_OUTBOX_CAP;
        client->out_count--;
        pthread_mutex_unlock(&client->out_mutex);

        enet_client_dispatch(client, &msg);
        free(msg.payload);
    }
}

static int enet_client_handle_event(ctm_enet_client_t *client, ENetEvent *event) {
    switch (event->type) {
        case ENET_EVENT_TYPE_CONNECT:
            client->peer = event->peer;
            client->connected = 1;
            return 0;
        case ENET_EVENT_TYPE_RECEIVE:
            enet_client_ingest(client, event->packet->data, event->packet->dataLength);
            enet_packet_destroy(event->packet);
            return 0;
        case ENET_EVENT_TYPE_DISCONNECT:
            client->connected = 0;
            client->peer = NULL;
            return -1;
        default:
            return 0;
    }
}

int enet_client_connect(ctm_enet_client_t *client, const char *host, int port, unsigned int timeout_ms) {
    if (!client || !client->host || !host) {
        return -1;
    }

    ENetAddress address;
    memset(&address, 0, sizeof(address));
    if (enet_address_set_host(&address, host) != 0) {
        return -1;
    }
    if (enet_address_set_port(&address, (enet_uint16) port) != 0) {
        return -1;
    }

    client->peer = enet_host_connect(client->host, &address, CTM_ENET_CHANNEL_COUNT, 0);
    if (!client->peer) {
        return -1;
    }
    client->connected = 0;

    uint64_t start = enet_now_us();
    for (;;) {
        ENetEvent event;
        int rc = enet_host_service(client->host, &event, 10);
        if (rc < 0) {
            break;
        }
        if (rc > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
            client->peer = event.peer;
            client->connected = 1;
            return 0;
        }
        if (rc > 0) {
            (void) enet_client_handle_event(client, &event);
            if (client->connected) {
                return 0;
            }
        }
        if (enet_now_us() - start >= (uint64_t) timeout_ms * 1000ull) {
            break;
        }
    }

    if (client->peer) {
        enet_peer_reset(client->peer);
        client->peer = NULL;
    }
    client->connected = 0;
    enet_client_reset_queues(client);
    return -1;
}

void enet_client_disconnect(ctm_enet_client_t *client) {
    if (!client) {
        return;
    }
    if (client->peer && client->connected) {
        enet_peer_disconnect(client->peer, 0);
        uint64_t start = enet_now_us();
        for (;;) {
            ENetEvent event;
            int rc = enet_host_service(client->host, &event, 10);
            if (rc > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    client->peer = NULL;
                    break;
                }
            }
            if (rc < 0 || enet_now_us() - start >= 200000ull) {
                break;
            }
        }
    }
    if (client->peer) {
        enet_peer_reset(client->peer);
        client->peer = NULL;
    }
    client->connected = 0;
    enet_client_reset_queues(client);
}

int enet_client_connected(const ctm_enet_client_t *client) {
    return client && client->connected;
}

int enet_client_service(ctm_enet_client_t *client, unsigned int timeout_ms) {
    if (!client || !client->host) {
        return -1;
    }
    int dropped = 0;

    enet_client_flush_outbox(client);

    ENetEvent event;
    int rc = enet_host_service(client->host, &event, timeout_ms);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        if (enet_client_handle_event(client, &event) < 0) {
            dropped = 1;
        }
        while (enet_host_check_events(client->host, &event) > 0) {
            if (enet_client_handle_event(client, &event) < 0) {
                dropped = 1;
            }
        }
    }

    enet_client_flush_outbox(client);
    return dropped ? -1 : 0;
}

int enet_client_send_msg(ctm_enet_client_t *client, uint16_t type, uint32_t flags, uint32_t request_id,
                         const void *payload, size_t len) {
    if (!client || !client->connected) {
        return -1;
    }
    if (len > CTMB_MAX_PAYLOAD) {
        return -1;
    }

    uint8_t *copy = NULL;
    if (len) {
        copy = (uint8_t *) malloc(len);
        if (!copy) {
            return -1;
        }
        memcpy(copy, payload, len);
    }

    pthread_mutex_lock(&client->out_mutex);
    if (client->out_count >= CTM_ENET_OUTBOX_CAP) {
        free(client->outbox[client->out_head].payload);
        client->outbox[client->out_head].payload = NULL;
        client->out_head = (client->out_head + 1) % CTM_ENET_OUTBOX_CAP;
        client->out_count--;
    }
    int idx = (client->out_head + client->out_count) % CTM_ENET_OUTBOX_CAP;
    ctmb_header_t *h = &client->outbox[idx].header;
    memset(h, 0, sizeof(*h));
    h->magic = CTMB_MAGIC;
    h->version = CTMB_VERSION;
    h->type = type;
    h->flags = flags;
    h->sequence = ++client->send_sequence;
    h->timestamp_us = enet_now_us();
    h->request_id = request_id;
    h->payload_len = (uint32_t) len;
    client->outbox[idx].payload = copy;
    client->out_count++;
    pthread_mutex_unlock(&client->out_mutex);
    return 0;
}

int enet_client_recv_msg(ctm_enet_client_t *client, ctmb_header_t *h, uint8_t **payload) {
    if (!client || !h || !payload) {
        return 0;
    }
    *payload = NULL;
    pthread_mutex_lock(&client->in_mutex);
    if (client->in_count <= 0) {
        pthread_mutex_unlock(&client->in_mutex);
        return 0;
    }
    *h = client->inbox[client->in_head].header;
    *payload = client->inbox[client->in_head].payload;
    client->inbox[client->in_head].payload = NULL;
    client->in_head = (client->in_head + 1) % CTM_ENET_INBOX_CAP;
    client->in_count--;
    pthread_mutex_unlock(&client->in_mutex);
    return 1;
}
