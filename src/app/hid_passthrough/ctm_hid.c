#include "ctm_hid.h"

#include <string.h>
#include <sys/ioctl.h>

#ifndef HIDIOCGRDESCSIZE
#define HIDIOCGRDESCSIZE _IOR('H', 0x01, int)
#endif

#ifdef HIDRAW_MAX_DESCRIPTOR_SIZE
typedef struct hidraw_report_descriptor ctm_rd_t;
#else
struct ctm_rd_local {
    uint32_t size;
    unsigned char value[4096];
};
typedef struct ctm_rd_local ctm_rd_t;
#ifndef HIDIOCGRDESC
#define HIDIOCGRDESC _IOR('H', 0x02, ctm_rd_t)
#endif
#endif

uint32_t hid_item_u32(const uint8_t *p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && i < 4; i++) {
        v |= ((uint32_t) p[i]) << (i * 8);
    }
    return v;
}

void derive_report_lengths(const uint8_t *desc, uint32_t desc_len, ctmb_device_caps_t *caps) {
    uint32_t report_size = 0, report_count = 0, report_id = 0;
    uint32_t max_in = 0, max_out = 0, max_feature = 0;
    for (uint32_t i = 0; i < desc_len;) {
        uint8_t b = desc[i++];
        if (b == 0xfe) {
            if (i + 2 > desc_len) {
                break;
            }
            uint8_t long_len = desc[i++];
            i++;
            if (i + long_len > desc_len) {
                break;
            }
            i += long_len;
            continue;
        }
        uint32_t size_code = b & 0x03;
        uint32_t item_len = size_code == 3 ? 4 : size_code;
        uint8_t type = (uint8_t) ((b >> 2) & 0x03);
        uint8_t tag = (uint8_t) ((b >> 4) & 0x0f);
        if (i + item_len > desc_len) {
            break;
        }
        uint32_t value = hid_item_u32(desc + i, item_len);
        i += item_len;

        if (type == 1) {
            if (tag == 7) {
                report_size = value;
            } else if (tag == 8) {
                report_id = value & 0xff;
            } else if (tag == 9) {
                report_count = value;
            }
        } else if (type == 0 && (tag == 8 || tag == 9 || tag == 11)) {
            uint32_t bits = report_size * report_count;
            uint32_t bytes = (bits + 7) / 8 + (report_id ? 1 : 0);
            if (tag == 8 && bytes > max_in) {
                max_in = bytes;
            } else if (tag == 9 && bytes > max_out) {
                max_out = bytes;
            } else if (tag == 11 && bytes > max_feature) {
                max_feature = bytes;
            }
        }
    }
    if (max_in && max_in < 65536) {
        caps->input_report_len = (uint16_t) max_in;
    }
    if (max_out && max_out < 65536) {
        caps->output_report_len = (uint16_t) max_out;
    }
    if (max_feature && max_feature < 65536) {
        caps->feature_report_len = (uint16_t) max_feature;
    }
}

void ctm_hid_top_usage(const uint8_t *desc, uint32_t desc_len, uint16_t *usage_page, uint16_t *usage,
                       uint32_t *max_report_bytes) {
    uint16_t up = 0, us = 0;
    int have_up = 0, have_us = 0;
    uint32_t report_size = 0, report_count = 0, report_id = 0, max_bytes = 0;
    for (uint32_t i = 0; i < desc_len;) {
        uint8_t b = desc[i++];
        if (b == 0xfe) {
            if (i + 2 > desc_len) {
                break;
            }
            uint8_t long_len = desc[i++];
            i++;
            if (i + long_len > desc_len) {
                break;
            }
            i += long_len;
            continue;
        }
        uint32_t size_code = b & 0x03;
        uint32_t item_len = size_code == 3 ? 4 : size_code;
        uint8_t type = (uint8_t) ((b >> 2) & 0x03);
        uint8_t tag = (uint8_t) ((b >> 4) & 0x0f);
        if (i + item_len > desc_len) {
            break;
        }
        uint32_t value = hid_item_u32(desc + i, item_len);
        i += item_len;

        if (type == 1) {
            if (tag == 0 && !have_up) {
                up = (uint16_t) value;
                have_up = 1;
            } else if (tag == 7) {
                report_size = value;
            } else if (tag == 8) {
                report_id = value & 0xff;
            } else if (tag == 9) {
                report_count = value;
            }
        } else if (type == 2) {
            if (tag == 0 && !have_us) {
                us = (uint16_t) value;
                have_us = 1;
            }
        } else if (type == 0 && (tag == 8 || tag == 9 || tag == 11)) {
            uint32_t bits = report_size * report_count;
            uint32_t bytes = (bits + 7) / 8 + (report_id ? 1 : 0);
            if (bytes > max_bytes) {
                max_bytes = bytes;
            }
        }
    }
    if (usage_page) {
        *usage_page = up;
    }
    if (usage) {
        *usage = us;
    }
    if (max_report_bytes) {
        *max_report_bytes = max_bytes;
    }
}

const char *ctm_hid_usage_label(uint16_t usage_page, uint16_t usage) {
    if (usage_page >= 0xff00) {
        return "vendor";
    }
    if (usage_page == 0x01) {
        switch (usage) {
            case 0x02:
                return "mouse";
            case 0x04:
                return "joystick";
            case 0x05:
                return "gamepad";
            case 0x06:
                return "keyboard";
            case 0x07:
                return "keypad";
            case 0x08:
                return "multiaxis";
            case 0x80:
                return "system";
            default:
                return "desktop";
        }
    }
    if (usage_page == 0x0b) {
        return "telephony";
    }
    if (usage_page == 0x0c) {
        return "consumer";
    }
    if (usage_page == 0x0d) {
        return "digitizer";
    }
    return "other";
}

uint32_t read_report_descriptor(int fd, uint8_t *out, uint32_t out_cap) {
    int desc_size = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) != 0 || desc_size <= 0) {
        return 0;
    }
    if ((uint32_t) desc_size > out_cap) {
        desc_size = (int) out_cap;
    }
    ctm_rd_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.size = (uint32_t) desc_size;
    if (ioctl(fd, HIDIOCGRDESC, &rd) != 0) {
        return 0;
    }
    memcpy(out, rd.value, (uint32_t) desc_size);
    return (uint32_t) desc_size;
}
