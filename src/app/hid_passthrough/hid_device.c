#include "hid_device.h"

#include "ctm_hid.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo {
    unsigned int bustype;
    short vendor;
    short product;
};
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif
#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif
#ifndef HIDIOCGRAWPHYS
#define HIDIOCGRAWPHYS(len) _IOC(_IOC_READ, 'H', 0x05, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif

#define LG_VENDOR_ID 0x005du

static void hid_device_release_grabs(hid_device_t *dev) {
    for (int i = 0; i < dev->grab_count; i++) {
        if (dev->grab_fds[i] >= 0) {
            ioctl(dev->grab_fds[i], EVIOCGRAB, 0);
            close(dev->grab_fds[i]);
            dev->grab_fds[i] = -1;
        }
    }
    dev->grab_count = 0;
}

static void hid_device_grab_evdev(hid_device_t *dev) {
    if (!dev->phys[0]) {
        return;
    }
    for (int i = 0; i < 32 && dev->grab_count < HID_PT_MAX_GRAB_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        char phys[256] = {0};
        if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) >= 0 && strcmp(phys, dev->phys) == 0) {
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                dev->grab_fds[dev->grab_count++] = fd;
                commons_log_info("HidPassthrough", "Grabbed %s for %s", path, dev->path);
                continue;
            }
        }
        close(fd);
    }
}

bool hid_device_is_gamepad_candidate(uint16_t vendor_id, uint16_t usage_page, uint16_t usage) {
    if (vendor_id == LG_VENDOR_ID) {
        return false;
    }
    if (usage_page == 0x01 && (usage == 0x04 || usage == 0x05)) {
        return true;
    }
    if (usage_page == 0x01 && (usage == 0x02 || usage == 0x06)) {
        return false;
    }
    if (usage_page >= 0xff00) {
        return true;
    }
    if (usage_page == 0x01 && usage == 0x08) {
        return true;
    }
    /* Descriptor parse can fail on webOS; accept common controller vendors anyway. */
    if (usage_page == 0 && usage == 0) {
        switch (vendor_id) {
            case 0x045e: /* Microsoft */
            case 0x054c: /* Sony */
            case 0x057e: /* Nintendo */
            case 0x04b4: /* Flydigi */
            case 0x3537: /* Gamesir */
            case 0x2dc8: /* 8BitDo */
            case 0x0e6f: /* PDP */
            case 0x1532: /* Razer */
            case 0x0738: /* Mad Catz */
            case 0x046d: /* Logitech */
            case 0x20d6: /* PowerA */
            case 0x24c6: /* PowerA / licensed */
            case 0x2563: /* 8BitDo alt */
                return true;
            default:
                break;
        }
    }
    return false;
}

int hid_device_gamepad_score(uint16_t vendor_id, uint16_t usage_page, uint16_t usage) {
    if (!hid_device_is_gamepad_candidate(vendor_id, usage_page, usage)) {
        return -1;
    }
    if (usage_page == 0x01 && usage == 0x05) {
        return 100;
    }
    if (usage_page == 0x01 && usage == 0x04) {
        return 90;
    }
    if (usage_page >= 0xff00) {
        return 50;
    }
    if (usage_page == 0x01 && usage == 0x08) {
        return 40;
    }
    (void) vendor_id;
    return 10;
}

int hid_device_open(hid_device_t *dev, const char *path) {
    memset(dev, 0, sizeof(*dev));
    for (int i = 0; i < HID_PT_MAX_GRAB_FDS; i++) {
        dev->grab_fds[i] = -1;
    }
    strncpy(dev->path, path, sizeof(dev->path) - 1);

    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fd = open(path, O_RDONLY | O_NONBLOCK);
    }
    if (fd < 0) {
        return -1;
    }
    dev->hidraw_fd = fd;

    struct hidraw_devinfo info = {0};
    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        dev->caps.bus = (uint16_t) info.bustype;
        dev->caps.vendor_id = (uint16_t) info.vendor;
        dev->caps.product_id = (uint16_t) info.product;
        dev->vendor_id = dev->caps.vendor_id;
        dev->product_id = dev->caps.product_id;
    }

    char name[64] = {0};
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) >= 0) {
        strncpy(dev->caps.product, name, sizeof(dev->caps.product) - 1);
    }
    if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(dev->phys) - 1), dev->phys) >= 0) {
        /* phys used for evdev grab */
    }
    strncpy(dev->caps.path, path, sizeof(dev->caps.path) - 1);

    dev->report_desc_len = read_report_descriptor(fd, dev->report_desc, sizeof(dev->report_desc));
    if (dev->report_desc_len) {
        derive_report_lengths(dev->report_desc, dev->report_desc_len, &dev->caps);
        ctm_hid_top_usage(dev->report_desc, dev->report_desc_len, &dev->usage_page, &dev->usage, NULL);
    }

    if (!hid_device_is_gamepad_candidate(dev->vendor_id, dev->usage_page, dev->usage)) {
        commons_log_info("HidPassthrough", "Skipping %s (%04x:%04x %s) — not a gamepad interface",
                         path, dev->vendor_id, dev->product_id,
                         ctm_hid_usage_label(dev->usage_page, dev->usage));
        close(fd);
        dev->hidraw_fd = -1;
        return -1;
    }

    hid_device_grab_evdev(dev);
    ctmb_hid_transport_init(&dev->transport, &dev->transport_ctx);
    return 0;
}

void hid_device_close(hid_device_t *dev) {
    hid_transport_disconnect(&dev->transport);
    ctmb_hid_transport_destroy(&dev->transport_ctx);
    hid_device_release_grabs(dev);
    if (dev->hidraw_fd >= 0) {
        close(dev->hidraw_fd);
        dev->hidraw_fd = -1;
    }
    dev->connected = false;
    dev->hello_sent = false;
}

int hid_device_send_hello(hid_device_t *dev) {
    ctmb_hid_descriptor_info_t desc_info;
    memset(&desc_info, 0, sizeof(desc_info));
    desc_info.report_descriptor_len = dev->report_desc_len;

    uint32_t hello_len = (uint32_t) (sizeof(dev->caps) + sizeof(desc_info) + dev->report_desc_len);
    uint8_t *hello = (uint8_t *) malloc(hello_len);
    if (!hello) {
        return -1;
    }
    memcpy(hello, &dev->caps, sizeof(dev->caps));
    memcpy(hello + sizeof(dev->caps), &desc_info, sizeof(desc_info));
    if (dev->report_desc_len) {
        memcpy(hello + sizeof(dev->caps) + sizeof(desc_info), dev->report_desc, dev->report_desc_len);
    }

    int rc = hid_transport_send_msg(&dev->transport, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, hello_len);
    free(hello);
    if (rc == 0) {
        dev->hello_sent = true;
        commons_log_info("HidPassthrough", "HELLO sent for %s vid=%04x pid=%04x", dev->path, dev->vendor_id,
                         dev->product_id);
    }
    return rc;
}

int hid_device_read_input(hid_device_t *dev, uint8_t *buf, size_t buf_len) {
    if (dev->hidraw_fd < 0) {
        return -1;
    }
    ssize_t r = read(dev->hidraw_fd, buf, buf_len);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
        return 0;
    }
    return (int) r;
}

int hid_device_handle_host_message(hid_device_t *dev, const ctmb_header_t *hdr, const uint8_t *payload) {
    switch (hdr->type) {
        case CTMB_MSG_OUTPUT_REPORT:
            if (hdr->payload_len && dev->hidraw_fd >= 0) {
                ssize_t w = write(dev->hidraw_fd, payload, hdr->payload_len);
                if (w < 0) {
                    commons_log_warn("HidPassthrough", "hidraw write failed: %s", strerror(errno));
                }
            }
            return 0;
        case CTMB_MSG_FEATURE_GET:
            if (hdr->payload_len >= 1 && dev->hidraw_fd >= 0) {
                uint8_t rb[HID_PT_MAX_REPORT];
                size_t report_len = hdr->payload_len;
                if (report_len > sizeof(rb)) {
                    report_len = sizeof(rb);
                }
                memset(rb, 0, sizeof(rb));
                memcpy(rb, payload, report_len);
                int n = ioctl(dev->hidraw_fd, HIDIOCGFEATURE(report_len), rb);
                if (n >= 0) {
                    return hid_transport_send_msg(&dev->transport, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK,
                                                  hdr->request_id, rb, (uint32_t) n);
                }
                return hid_transport_send_msg(&dev->transport, CTMB_MSG_FEATURE_REPORT, 0, hdr->request_id, NULL, 0);
            }
            return 0;
        case CTMB_MSG_FEATURE_SET: {
            int ok = 0;
            if (hdr->payload_len && dev->hidraw_fd >= 0) {
                int n = ioctl(dev->hidraw_fd, HIDIOCSFEATURE(hdr->payload_len), payload);
                ok = n >= 0;
            }
            return hid_transport_send_msg(&dev->transport, CTMB_MSG_FEATURE_REPORT, ok ? CTMB_FLAG_OK : 0,
                                          hdr->request_id, NULL, 0);
        }
        case CTMB_MSG_HOST_CONFIG:
            return 0;
        default:
            return 0;
    }
}
