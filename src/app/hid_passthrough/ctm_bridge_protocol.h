#ifndef CTM_BRIDGE_PROTOCOL_H
#define CTM_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define CTMB_MAGIC 0x54424d43u
#define CTMB_VERSION 1u
#define CTMB_FLAG_OK 0x00000001u
#define CTMB_FLAG_PACED 0x00000002u
#define CTMB_MAX_PAYLOAD 65536u

enum ctmb_message_type {
    CTMB_MSG_HELLO = 1,
    CTMB_MSG_HOST_CONFIG = 2,
    CTMB_MSG_INPUT_REPORT = 3,
    CTMB_MSG_OUTPUT_REPORT = 4,
    CTMB_MSG_FEATURE_GET = 5,
    CTMB_MSG_FEATURE_REPORT = 6,
    CTMB_MSG_LOG = 7,
    CTMB_MSG_ERROR = 8,
    CTMB_MSG_FEATURE_SET = 9,
    CTMB_MSG_ENUM = 10
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t request_id;
    uint32_t payload_len;
} ctmb_header_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    uint16_t bus;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
    uint16_t flags;
    char path[64];
    char serial[64];
    char product[64];
    char manufacturer[64];
} ctmb_device_caps_t;

typedef struct {
    uint32_t report_descriptor_len;
    uint8_t reserved[28];
} ctmb_hid_descriptor_info_t;

typedef struct {
    uint32_t bt_pace_us;
    uint16_t input_report_len;
    uint16_t output_report_len;
    uint16_t feature_report_len;
    uint8_t paced_report_count;
    uint8_t paced_report_ids[16];
    uint8_t reserved[31];
} ctmb_host_config_t;

typedef struct {
    uint16_t descriptors_len;
    uint8_t iface_count;
    uint8_t full_speed;
    uint8_t reserved[28];
} ctmb_enum_info_t;

typedef struct {
    uint8_t interface_number;
    uint8_t iface_class;
    uint16_t report_desc_len;
} ctmb_enum_iface_t;
#pragma pack(pop)

#endif
