#ifndef HID_DEVICE_H
#define HID_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "ctm_bridge_protocol.h"
#include "hid_transport.h"

#define HID_PT_MAX_GRAB_FDS 8
#define HID_PT_MAX_REPORT 4096
#define HID_PT_PATH_LEN 64

typedef struct hid_device {
    char path[HID_PT_PATH_LEN];
    char phys[256];
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usage_page;
    uint16_t usage;
    int hidraw_fd;
    int grab_fds[HID_PT_MAX_GRAB_FDS];
    int grab_count;
    uint8_t report_desc[HID_PT_MAX_REPORT];
    uint32_t report_desc_len;
    ctmb_device_caps_t caps;
    hid_transport_t transport;
    ctmb_hid_transport_ctx_t transport_ctx;
    bool connected;
    bool hello_sent;
} hid_device_t;

bool hid_device_is_gamepad_candidate(uint16_t vendor_id, uint16_t usage_page, uint16_t usage);

/** Higher score = prefer this hidraw when a composite exposes multiple nodes. */
int hid_device_gamepad_score(uint16_t vendor_id, uint16_t usage_page, uint16_t usage);

int hid_device_open(hid_device_t *dev, const char *path);

void hid_device_close(hid_device_t *dev);

int hid_device_send_hello(hid_device_t *dev);

int hid_device_read_input(hid_device_t *dev, uint8_t *buf, size_t buf_len);

int hid_device_handle_host_message(hid_device_t *dev, const ctmb_header_t *hdr, const uint8_t *payload);

#endif
