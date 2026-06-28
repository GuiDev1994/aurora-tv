/* Controller pump + lifecycle + factory (D2 stage 2).
 *
 * One isolated bridging session per controller: its own reader + session
 * threads, HID fd, ctm_transport, settings, and per-MAC log. This is the merge
 * of tv_bridge_worker.c (DS pump: 2 threads, pacing, evdev grab, HELLO/
 * HOST_CONFIG handshake) and hidraw_bridge.c (verbatim relay). Per-type
 * behaviour is preserved via ops flags (needs_host_config / grab_evdev /
 * request_bt_mode) and the patch_output hook. */

#define _GNU_SOURCE

#include "ctm_controller.h"
#include "ctm_hid.h"   /* read_report_descriptor, derive_report_lengths */
#include "ctm_state.h" /* composite_usb_device_dir_by_busid */
#include "ds5_acl_tx.h"
#include "ds5_hidfd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#include <linux/input.h>
#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#define BTN_EAST  0x131
#define BTN_NORTH 0x133
#define BTN_WEST  0x134
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#endif
#endif

#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo { unsigned int bustype; short vendor; short product; };
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif
#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif
#ifndef ABS_HAT0X
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#endif
#ifndef FF_RUMBLE
#define FF_RUMBLE 0x50
#endif

#define MAX_REPORT 4096
#define MAX_REPORT_DESCRIPTOR 4096
#define PACED_QUEUE_CAP 32
#define MAX_EVDEV_GRABS 16
#define BUS_BLUETOOTH 0x05
#define BUS_USB 0x03

typedef struct { uint8_t data[MAX_REPORT]; size_t len; } queued_report_t;
typedef struct { int fd; char path[64]; } evdev_grab_t;

/* Composite (puck): one forwarded sibling HID interface. The primary is c->hid_fd;
 * comp[] are the rest. Each input report is tagged with the interface's IN endpoint. */
typedef struct {
    ctm_controller_t *c;
    int fd;
    uint8_t in_ep;
    uint8_t out_ep;
    uint8_t iface;
    pthread_t thread;
    int started;
} comp_iface_t;

struct ctm_controller {
    const ctm_controller_ops_t *ops;
    ctm_controller_dev_t dev;
    unsigned int vid_num;
    unsigned int pid_num;
    char host[64];
    int port;

    ctm_transport_t xport;
    ctm_enet_client_t *enet;        /* process-owned client; borrowed by xport */
    int hid_fd;
    ds5_acl_tx_t *acl_tx;          /* DS5 raw-ACL forwarder (NULL = hidraw only) */
    int plc_enabled;
    int plc_have;
    int plc_repeat;
    uint16_t plc_audio_len;
    uint8_t plc_audio[260];
    unsigned long st_audio_omit;
    unsigned long st_audio_conceal;
    unsigned long st_audio_capdrop;
    uint64_t plc_log_next_us;
    unsigned long st_hid_ok, st_hid_eagain, st_hid_recovered, st_hid_dropped;
    int hid_wait_ms;
    int dedup_enabled;
    size_t last31_len;
    uint8_t last31[80];
    unsigned long st_dedup_skipped;
    int wake_pipe[2];

    pthread_t session_thread;
    int session_started;
    pthread_t input_thread;
    int input_thread_started;
    volatile int stop;

    pthread_mutex_t hid_mutex;
    pthread_mutex_t settings_mutex;
    tv_bridge_worker_settings_t settings;

    evdev_grab_t evdev_grabs[MAX_EVDEV_GRABS];
    int evdev_grab_count;

    FILE *log;

    /* Live status for the UI panel. Counters are single-writer (reports_in:
     * reader thread; reports_out: session thread) and read advisorily;
     * connected/transport/last_event are guarded by status_mutex. */
    pthread_mutex_t status_mutex;
    volatile int st_connected;
    volatile int st_transport_enet;
    volatile unsigned long st_reports_in;
    volatile unsigned long st_reports_out;
    volatile unsigned long st_coalesced;   /* input reports dropped by per-burst coalescing */
    char st_last_event[96];

    uint8_t *enum_payload;           /* composite: forwarded enumeration (CTMB_MSG_ENUM) */
    int enum_payload_len;
    comp_iface_t comp[15];           /* composite: the non-primary HID interfaces */
    int comp_count;
    uint8_t primary_in_ep;           /* the primary hidraw's IN endpoint (input tag) */
    uint8_t primary_out_ep;          /* the primary hidraw's OUT endpoint (output route) */
    uint8_t primary_iface;           /* the primary hidraw's USB interface number */
    volatile int comp_run;           /* gates the sibling reader threads */
    int evdev_gamepad_fd;            /* Flydigi XInput: xpad evdev feeder */
    pthread_t evdev_gamepad_thread;
    int evdev_gamepad_started;
    uint8_t xpad_in_ep;              /* IN endpoint of the xpad-claimed iface */
    uint8_t xpad_out_ep;             /* OUT endpoint of the xpad-claimed iface */
    int xpad_ff_effect_id;           /* evdev FF_RUMBLE effect slot */
    int flydigi_xinput_evdev_only;   /* gamepad via evdev only (no hidraw on bus) */
    int dummy_hid_pipe_wr;           /* write end of placeholder hid pipe */
};

/* ENet is process-global; init once for all controllers, never deinit. */
static pthread_once_t g_enet_once = PTHREAD_ONCE_INIT;
static int g_enet_ready = 0;
static void enet_global_init_once(void)
{
    if (enet_client_global_init() == 0) g_enet_ready = 1;
    else fprintf(stderr, "controller: enet_initialize failed; ENet disabled, TCP only\n");
}

/* Monotonic clock in microseconds. When: pacing schedules + handshake timeout. */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Optional UI log sink, set by the app, so controller events also appear in the
 * on-screen console. NULL => file + stderr only. */
static void (*g_log_sink)(const char *line);

void ctm_controller_set_log_sink(void (*sink)(const char *line))
{
    g_log_sink = sink;
}

/* Per-controller log: writes to its MAC-named file (if open), stderr, and the
 * UI sink (if set), each line prefixed with the controller kind. When:
 * throughout a controller's lifetime (any of its threads). */
static void ctl_log(ctm_controller_t *c, const char *fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    const char *kind = c->ops ? c->ops->kind : "ctl";
    fprintf(stderr, "[%s] %s\n", kind, body);
    if (c->log) {
        fprintf(c->log, "%s\n", body);
        fflush(c->log);
    }
    pthread_mutex_lock(&c->status_mutex);
    snprintf(c->st_last_event, sizeof(c->st_last_event), "%s", body);
    pthread_mutex_unlock(&c->status_mutex);
    if (g_log_sink) {
        char line[600];
        snprintf(line, sizeof(line), "%s: %s", kind, body);
        g_log_sink(line);
    }
}

static void ctl_set_rt_prio(ctm_controller_t *c, const char *who, int prio)
{
    const char *env = getenv("CTM_RT");
    if (env && strcmp(env, "0") == 0) {
        ctl_log(c, "%s RT disabled (CTM_RT=0)", who);
        return;
    }
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        ctl_log(c, "%s SCHED_FIFO prio=%d", who, prio);
    } else {
        int e = errno;
        (void)setpriority(PRIO_PROCESS, 0, -20);
        ctl_log(c, "%s SCHED_FIFO denied errno=%d -> nice -20", who, e);
    }
}

static void acl_log_cb(void *ctx, const char *msg)
{
    ctl_log((ctm_controller_t *)ctx, "[acl] %s", msg);
}

/* Send one framed CTMB message over this controller's transport. When: the
 * reader thread (input reports), the session thread (HELLO + feature replies). */
static int c_send(ctm_controller_t *c, uint16_t type, uint32_t flags,
                  uint32_t request_id, const void *payload, size_t len)
{
    return ctm_transport_send_msg(&c->xport, type, flags, request_id, payload, len);
}

/* Pop one received message (1=got / 0=none / -1=dropped). When: the session
 * thread loop and the handshake wait. */
static int c_recv(ctm_controller_t *c, ctmb_header_t *h, uint8_t **payload)
{
    return ctm_transport_recv_msg(&c->xport, h, payload);
}

/* Compare a hex-string sysfs attr to a numeric vid/pid. When: evdev matching. */
static int hex_equals(const char *text, unsigned int value)
{
    if (!text || !text[0]) return 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 16);
    return end != text && parsed == value;
}

/* Sony feature-0x05 "full BT mode" probe. When: at HID open, DS only
 * (gated by ops->request_bt_mode). */
static void request_full_bt_mode(int fd)
{
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        fprintf(stderr, "controller: feature 0x05 failed errno=%d\n", errno);
    }
}

/* CRC32 (reflected, poly 0xedb88320) step. When: ctm_bt_sign_output only. */
static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

/* Append the Sony BT HID output-report CRC32 (seed 0xa2) into the trailing 4
 * bytes. When: a DS patch_output hook, after rewriting a report. */
void ctm_bt_sign_output(uint8_t *data, size_t len)
{
    if (!data || len < 8) return;
    uint8_t seed = 0xa2;
    uint32_t crc = crc32_step(0xffffffffu, &seed, 1);
    crc = ~crc32_step(crc, data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xffu);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xffu);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xffu);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xffu);
}

static void flydigi_fill_caps_identity(const char *usb_busid, char *mfg, size_t mfg_len,
                                       char *product, size_t product_len);

/* Open the controller's hidraw node (ops->select_node or dev.path), validate
 * vid/pid, fill caps + report descriptor. When: once by session_main before
 * the connect loop. Returns the fd, or -1. */
static int open_hid(ctm_controller_t *c, ctmb_device_caps_t *caps,
                    uint8_t *report_desc, uint32_t *report_desc_len)
{
    char node[64];
    const char *path = c->dev.path;
    if (c->ops->select_node && c->ops->select_node(&c->dev, node, sizeof(node)) == 0 && node[0]) {
        path = node;
    }

    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fd = ds5_hidfd_request(path);
        if (fd < 0) {
            return -1;
        }
        ctl_log(c, "hid fd via root broker for %s (jail node absent)", path);
    }

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        close(fd);
        return -1;
    }
    unsigned int vid = (unsigned short)info.vendor;
    unsigned int pid = (unsigned short)info.product;
    if (!(c->ops && c->ops->composite) &&
        ((c->vid_num && vid != c->vid_num) || (c->pid_num && pid != c->pid_num))) {
        close(fd);
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = (uint16_t)vid;
    caps->product_id = (uint16_t)pid;
    caps->bus = info.bustype ? (uint16_t)info.bustype : BUS_BLUETOOTH;
    caps->input_report_len = 1024;
    caps->output_report_len = 1024;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->path, sizeof(caps->path), "%s", path);
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);
    snprintf(caps->manufacturer, sizeof(caps->manufacturer), "hidraw");
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(caps->product) - 1), caps->product) < 0 ||
        caps->product[0] == '\0') {
        snprintf(caps->product, sizeof(caps->product), "hidraw");
    }
    if (c->ops && c->ops->composite && strcmp(c->ops->kind, "flydigi") == 0 &&
        c->dev.usb_busid[0]) {
        flydigi_fill_caps_identity(c->dev.usb_busid, caps->manufacturer, sizeof(caps->manufacturer),
                                     caps->product, sizeof(caps->product));
        if (c->vid_num) caps->vendor_id = (uint16_t)c->vid_num;
        if (c->pid_num) caps->product_id = (uint16_t)c->pid_num;
    }

    *report_desc_len = read_report_descriptor(fd, report_desc, MAX_REPORT_DESCRIPTOR);
    if (*report_desc_len) {
        derive_report_lengths(report_desc, *report_desc_len, caps);
        if (caps->input_report_len < 1024) caps->input_report_len = 1024;
        if (caps->output_report_len < 1024) caps->output_report_len = 1024;
    }

    if (c->ops->request_bt_mode) request_full_bt_mode(fd);
    return fd;
}

static int open_xpad_evdev_for_busid(const char *usb_busid, char *path_out, size_t path_len);

static void flydigi_fill_caps_identity(const char *usb_busid, char *mfg, size_t mfg_len,
                                       char *product, size_t product_len)
{
    char sysfs_mfg[64] = {0};
    char sysfs_prod[64] = {0};
    if (usb_busid && usb_busid[0] &&
        read_usb_identity_attrs(usb_busid, sysfs_mfg, sizeof(sysfs_mfg),
                                sysfs_prod, sizeof(sysfs_prod)) == 0) {
        if (sysfs_mfg[0] && mfg && mfg_len) {
            snprintf(mfg, mfg_len, "%s", sysfs_mfg);
        }
        if (sysfs_prod[0] && product && product_len) {
            if (contains_ci(sysfs_prod, "apex")) {
                snprintf(product, product_len, "Apex 4");
            } else if (contains_ci(sysfs_prod, "vader")) {
                snprintf(product, product_len, "Vader3");
            } else {
                snprintf(product, product_len, "%s", sysfs_prod);
            }
            return;
        }
    }
    if (mfg && mfg_len && !mfg[0]) {
        snprintf(mfg, mfg_len, "Flydigi");
    }
    if (product && product_len && !product[0]) {
        snprintf(product, product_len, "Apex 4");
    }
}

/* Flydigi XInput with no hidraw on the USB bus: caps + synthetic descriptor,
 * placeholder pipe for the input thread; gamepad input comes from xpad evdev. */
static int flydigi_open_xinput_evdev_only(ctm_controller_t *c, ctmb_device_caps_t *caps,
                                          uint8_t *report_desc, uint32_t *report_desc_len)
{
    if (!c || !caps || !report_desc || !report_desc_len) {
        return -1;
    }
    if (!flydigi_is_xinput_evdev_only_for_busid(c->dev.usb_busid)) {
        return -1;
    }

    char evdev_path[64];
    if (open_xpad_evdev_for_busid(c->dev.usb_busid, evdev_path, sizeof(evdev_path)) != 0) {
        ctl_log(c, "flydigi xinput evdev-only: xpad path missing busid=%s", c->dev.usb_busid);
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    caps->bus = BUS_USB;
    caps->input_report_len = 64;
    caps->output_report_len = 64;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);
    snprintf(caps->path, sizeof(caps->path), "%s", evdev_path);

    char usbdir[512];
    if (composite_usb_device_dir_by_busid(c->dev.usb_busid, usbdir, sizeof(usbdir)) == 0) {
        char path[600], v[16] = {0}, p[16] = {0};
        snprintf(path, sizeof(path), "%s/idVendor", usbdir);
        read_text_file(path, v, sizeof(v));
        snprintf(path, sizeof(path), "%s/idProduct", usbdir);
        read_text_file(path, p, sizeof(p));
        if (v[0]) {
            caps->vendor_id = (uint16_t)strtoul(v, NULL, 16);
        }
        if (p[0]) {
            caps->product_id = (uint16_t)strtoul(p, NULL, 16);
        }
    }
    if (!caps->vendor_id) {
        caps->vendor_id = (uint16_t)0x045e;
    }
    if (!caps->product_id) {
        caps->product_id = (uint16_t)0x028e;
    }

    flydigi_fill_caps_identity(c->dev.usb_busid, caps->manufacturer, sizeof(caps->manufacturer),
                                 caps->product, sizeof(caps->product));

    if (flydigi_xbox360_wired_rdesc_len > MAX_REPORT_DESCRIPTOR) {
        return -1;
    }
    memcpy(report_desc, flydigi_xbox360_wired_rdesc, flydigi_xbox360_wired_rdesc_len);
    *report_desc_len = (uint32_t)flydigi_xbox360_wired_rdesc_len;
    derive_report_lengths(report_desc, *report_desc_len, caps);
    if (caps->input_report_len < 64) {
        caps->input_report_len = 64;
    }
    if (caps->output_report_len < 64) {
        caps->output_report_len = 64;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        ctl_log(c, "flydigi xinput evdev-only: pipe failed errno=%d", errno);
        return -1;
    }
    (void)fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL, 0) | O_NONBLOCK);
    c->flydigi_xinput_evdev_only = 1;
    c->dummy_hid_pipe_wr = pipefd[1];
    ctl_log(c, "flydigi xinput evdev-only session busid=%s evdev=%s caps=%04x:%04x",
            c->dev.usb_busid, evdev_path, caps->vendor_id, caps->product_id);
    return pipefd[0];
}

/* Flydigi dongle in XInput mode with hidraw siblings: open a non-mouse hidraw
 * hidraw for the input thread and supply synthetic Xbox 360 wired caps/descriptor
 * for HELLO (identity still comes from sysfs). */
static int flydigi_open_xinput_handshake(ctm_controller_t *c, ctmb_device_caps_t *caps,
                                         uint8_t *report_desc, uint32_t *report_desc_len)
{
    if (!c || !caps || !report_desc || !report_desc_len) return -1;
    if (!flydigi_is_xinput_mode_for_busid(c->dev.usb_busid)) return -1;

    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = c->vid_num ? (uint16_t)c->vid_num : (uint16_t)0x04b4;
    caps->product_id = c->pid_num ? (uint16_t)c->pid_num : (uint16_t)0x2412;
    caps->bus = BUS_USB;
    caps->input_report_len = 64;
    caps->output_report_len = 64;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->serial, sizeof(caps->serial), "%s", c->dev.mac);

    flydigi_fill_caps_identity(c->dev.usb_busid, caps->manufacturer, sizeof(caps->manufacturer),
                                 caps->product, sizeof(caps->product));

    if (flydigi_xbox360_wired_rdesc_len > MAX_REPORT_DESCRIPTOR) return -1;
    memcpy(report_desc, flydigi_xbox360_wired_rdesc, flydigi_xbox360_wired_rdesc_len);
    *report_desc_len = (uint32_t)flydigi_xbox360_wired_rdesc_len;
    derive_report_lengths(report_desc, *report_desc_len, caps);
    if (caps->input_report_len < 64) caps->input_report_len = 64;
    if (caps->output_report_len < 64) caps->output_report_len = 64;

    char node[64] = {0};
    if (flydigi_handshake_hidraw_path_for_busid(c->dev.usb_busid, node, sizeof(node)) != 0 &&
        flydigi_hidraw_path_for_busid(c->dev.usb_busid, node, sizeof(node)) != 0) {
        ctl_log(c, "flydigi xinput mode: no hidraw for handshake (synthetic caps only)");
        return -1;
    }

    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ctl_log(c, "flydigi xinput handshake open failed path=%s errno=%d", node, errno);
        return -1;
    }
    snprintf(caps->path, sizeof(caps->path), "%s", node);
    ctl_log(c, "flydigi xinput mode: handshake via sysfs, gamepad via xpad evdev path=%s",
            node);
    return fd;
}

/* EVIOCGRAB the device's evdev nodes so webOS doesn't double-consume input.
 * When: at session start, BT/DS only (gated by ops->grab_evdev). */
static void grab_matching_evdev(ctm_controller_t *c)
{
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char vendor_path[160], product_path[160], vendor[32] = {0}, product[32] = {0};
        snprintf(vendor_path, sizeof(vendor_path), "/sys/class/input/%s/id/vendor", ent->d_name);
        snprintf(product_path, sizeof(product_path), "/sys/class/input/%s/id/product", ent->d_name);
        if (read_text_file(vendor_path, vendor, sizeof(vendor)) != 0 ||
            read_text_file(product_path, product, sizeof(product)) != 0 ||
            !hex_equals(vendor, c->vid_num) || !hex_equals(product, c->pid_num)) {
            continue;
        }
        char input_dir[160];
        snprintf(input_dir, sizeof(input_dir), "/sys/class/input/%s", ent->d_name);
        DIR *input = opendir(input_dir);
        if (!input) continue;
        struct dirent *child;
        while ((child = readdir(input)) != NULL && c->evdev_grab_count < MAX_EVDEV_GRABS) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            char dev_path[64];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", child->d_name);
            int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                int idx = c->evdev_grab_count++;
                c->evdev_grabs[idx].fd = fd;
                snprintf(c->evdev_grabs[idx].path, sizeof(c->evdev_grabs[idx].path), "%s", dev_path);
                ctl_log(c, "grabbed %s", dev_path);
            } else {
                close(fd);
            }
        }
        closedir(input);
    }
    closedir(dir);
}

/* Un-grab + close the evdev nodes. When: each time a session ends. */
static void release_evdev_grabs(ctm_controller_t *c)
{
    for (int i = 0; i < c->evdev_grab_count; ++i) {
        if (c->evdev_grabs[i].fd >= 0) {
            ioctl(c->evdev_grabs[i].fd, EVIOCGRAB, 0);
            close(c->evdev_grabs[i].fd);
            c->evdev_grabs[i].fd = -1;
        }
    }
    c->evdev_grab_count = 0;
}

/* Whether an outbound report must be rate-limited (PACED flag or a host_config
 * paced report id). When: per OUTPUT report in handle_message. */
static int should_pace(const ctmb_host_config_t *cfg, const ctmb_header_t *h,
                       const uint8_t *payload, size_t len)
{
    if ((h->flags & CTMB_FLAG_PACED) != 0) return 1;
    if (!payload || len == 0) return 0;
    for (int i = 0; i < cfg->paced_report_count && i < 16; i++) {
        if (payload[0] == cfg->paced_report_ids[i]) return 1;
    }
    return 0;
}

/* Enqueue a paced output report into the ring (drops oldest if full).
 * When: handle_message decides a report must be paced. */
static void queue_paced(queued_report_t *q, int *head, int *count,
                        const uint8_t *data, size_t len)
{
    if (len > MAX_REPORT) return;
    if (*count >= PACED_QUEUE_CAP) {
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
    }
    int idx = (*head + *count) % PACED_QUEUE_CAP;
    memcpy(q[idx].data, data, len);
    q[idx].len = len;
    (*count)++;
}

/* Thread-safe snapshot of the live settings. When: ctm_controller_get_settings
 * (so a patch_output hook can read current sliders without racing the UI). */
static tv_bridge_worker_settings_t copy_settings(ctm_controller_t *c)
{
    tv_bridge_worker_settings_t s;
    pthread_mutex_lock(&c->settings_mutex);
    s = c->settings;
    pthread_mutex_unlock(&c->settings_mutex);
    return s;
}

/* Returns nonzero to drop the report (patch consumed it), 0 to write. */
static int apply_output_settings(ctm_controller_t *c, uint8_t *data, size_t *len_io)
{
    if (!c->ops->patch_output) return 0;
    return c->ops->patch_output(c, data, len_io);
}

static void ds5_audio_plc(ctm_controller_t *c, uint8_t *data, size_t len)
{
    if (!c->plc_enabled || !data || len < 12 || data[0] != 0x36) {
        return;
    }

    size_t limit = len - 4;
    size_t pos = 2;
    int audio_present = 0;
    size_t haptic_pos = 0;
    size_t blocks_end = 2;
    while (pos + 2 <= limit) {
        uint8_t id = data[pos];
        size_t plen = data[pos + 1];
        size_t blen = plen + 2;
        if (id == 0 && plen == 0) {
            break;
        }
        if (blen > limit - pos) {
            return;
        }
        if (id >= 0x93 && id <= 0x96 && plen >= 100) {
            audio_present = 1;
            if (blen <= sizeof(c->plc_audio)) {
                memcpy(c->plc_audio, &data[pos], blen);
                c->plc_audio_len = (uint16_t)blen;
                c->plc_have = 1;
            }
        } else if (id == 0x92) {
            haptic_pos = pos;
        }
        pos += blen;
        blocks_end = pos;
    }
    if (audio_present) {
        c->plc_repeat = 0;
        return;
    }
    c->st_audio_omit++;
    if (!c->plc_have || haptic_pos == 0 || c->plc_repeat >= 3) {
        c->st_audio_capdrop++;
        return;
    }

    size_t al = c->plc_audio_len;
    if (blocks_end + al > limit) {
        c->st_audio_capdrop++;
        return;
    }
    memmove(&data[haptic_pos + al], &data[haptic_pos], blocks_end - haptic_pos);
    memcpy(&data[haptic_pos], c->plc_audio, al);
    ctm_bt_sign_output(data, len);
    c->plc_repeat++;
    c->st_audio_conceal++;
}

/* Patch (via the ops hook) then write one report to the device, mutex-guarded.
 * When: every direct OUTPUT write and every paced-queue drain. */
static int hid_write_report(ctm_controller_t *c, const uint8_t *data, size_t len)
{
    if (!c || c->hid_fd < 0 || !data || len == 0) return -1;
    uint8_t patched[MAX_REPORT];
    if (len > sizeof(patched)) return -1;
    memcpy(patched, data, len);
    size_t patched_len = len;
    if (apply_output_settings(c, patched, &patched_len)) {
        return 0;
    }
    ds5_audio_plc(c, patched, patched_len);
    if (c->dedup_enabled && patched_len >= 8 && patched[0] == 0x31) {
        size_t cmp = patched_len - 4;
        if (patched_len == c->last31_len &&
            memcmp(patched + 2, c->last31 + 2, cmp - 2) == 0) {
            c->st_dedup_skipped++;
            return 0;
        }
        if (patched_len <= sizeof(c->last31)) {
            memcpy(c->last31, patched, patched_len);
            c->last31_len = patched_len;
        }
    }
    if (c->acl_tx && patched_len > 0 && ds5_acl_is_injectable(patched[0])) {
        int rc = ds5_acl_tx_send(c->acl_tx, patched, patched_len);
        if (rc == DS5_ACL_TX_SENT) {
            c->st_reports_out++;
            return 0;
        }
    }
    pthread_mutex_lock(&c->hid_mutex);
    ssize_t n = write(c->hid_fd, patched, patched_len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        c->st_hid_eagain++;
        if (c->hid_wait_ms > 0) {
            struct pollfd pf;
            pf.fd = c->hid_fd;
            pf.events = POLLOUT;
            pf.revents = 0;
            if (poll(&pf, 1, c->hid_wait_ms) > 0 && (pf.revents & POLLOUT)) {
                n = write(c->hid_fd, patched, patched_len);
                if (n == (ssize_t)patched_len) {
                    c->st_hid_recovered++;
                }
            }
        }
    }
    pthread_mutex_unlock(&c->hid_mutex);
    if (n == (ssize_t)patched_len) {
        c->st_hid_ok++;
        c->st_reports_out++;
        return 0;
    }
    c->st_hid_dropped++;
    return -1;
}

/* Composite: write a report verbatim to a specific sibling fd (no output-setting
 * patching -- the composite is an identity passthrough). */
static int hid_write_fd_raw(ctm_controller_t *c, int fd, const uint8_t *data, size_t len)
{
    if (!c || fd < 0 || !data || len == 0 || len > MAX_REPORT) return -1;
    pthread_mutex_lock(&c->hid_mutex);
    ssize_t n = write(fd, data, len);
    pthread_mutex_unlock(&c->hid_mutex);
    if (n == (ssize_t)len) { c->st_reports_out++; return 0; }
    return -1;
}

/* Flush queued paced reports as their schedule comes due. When: top of every
 * session-loop tick. */
static void drain_paced(ctm_controller_t *c, queued_report_t *q, int *head, int *count,
                        uint64_t *next_us, uint32_t pace_us)
{
    uint64_t now = now_us();
    if (*count <= 0) { *next_us = 0; return; }
    if (*next_us == 0) *next_us = now;
    while (*count > 0 && now >= *next_us) {
        queued_report_t *r = &q[*head];
        (void)hid_write_report(c, r->data, r->len);
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
        if (pace_us == 0) pace_us = 10667;
        *next_us += pace_us;
        if (*next_us + pace_us < now) *next_us = now + pace_us;
        now = now_us();
    }
    if (*count <= 0) *next_us = 0;
}

/* Reader thread body: blocking-read hidraw input and forward it to the
 * transport. When: one per live session, started by run_session, stopped via
 * the wake pipe. */
/* --- Composite (puck): forward every HID interface ------------------------- */

/* Resolve the USB device dir (the one holding idVendor) for vid/pid via the
 * reliable /sys/class/input path -- the /sys/class/hidraw realpath is FLAKY in
 * the dev-mode jail, so the composite open must not depend on it. Mirrors
 * grab_matching_evdev / puck_usb_device_dir. 0 on success. */
static int resolve_usb_device_dir(unsigned int vid, unsigned int pid, char *out, size_t out_len)
{
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (strncmp(e->d_name, "input", 5) != 0) continue;
        char attr[256], v[32] = {0}, p[32] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", e->d_name);
        read_text_file(attr, v, sizeof(v));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", e->d_name);
        read_text_file(attr, p, sizeof(p));
        if (!hex_equals(v, vid) || !hex_equals(p, pid)) continue;
        char link[256], real[1024];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", e->d_name);
        if (!realpath(link, real)) continue;
        while (real[0]) {                       /* walk up to the USB device dir */
            char idf[1100];
            snprintf(idf, sizeof(idf), "%s/idVendor", real);
            if (access(idf, F_OK) == 0) { snprintf(out, out_len, "%s", real); rc = 0; break; }
            char *s = strrchr(real, '/'); if (!s || s == real) break; *s = '\0';
        }
    }
    closedir(d);
    return rc;
}

/* Read the IN (0x80 set) and OUT (0x80 clear) endpoint addresses of a USB
 * interface dir from its ep_* children. Leaves either at 0 if absent. */
static void iface_endpoints(const char *ifdir, uint8_t *in_ep, uint8_t *out_ep)
{
    DIR *d = opendir(ifdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ep_", 3) != 0) continue;
        char epf[1200], val[16] = {0};
        snprintf(epf, sizeof(epf), "%s/%s/bEndpointAddress", ifdir, e->d_name);
        if (read_text_file(epf, val, sizeof(val)) != 0) continue;
        unsigned int a = (unsigned int)strtoul(val, NULL, 16);
        if (a & 0x80u) { if (in_ep && *in_ep == 0) *in_ep = (uint8_t)a; }
        else { if (out_ep && *out_ep == 0) *out_ep = (uint8_t)a; }
    }
    closedir(d);
}

/* Find /dev/hidrawN under a USB interface dir (ifdir/<hid>/hidraw/hidrawN). */
static int find_hidraw_under(const char *ifdir, char *out, size_t outlen)
{
    DIR *d = opendir(ifdir); if (!d) return -1;
    struct dirent *e; int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (e->d_name[0] == '.') continue;
        char hp[1200]; snprintf(hp, sizeof(hp), "%s/%s/hidraw", ifdir, e->d_name);
        DIR *h = opendir(hp);
        if (h) {
            struct dirent *he;
            while ((he = readdir(h)) != NULL) {
                if (strncmp(he->d_name, "hidraw", 6) == 0) {
                    snprintf(out, outlen, "/dev/%s", he->d_name); rc = 0; break;
                }
            }
            closedir(h);
        }
    }
    closedir(d);
    return rc;
}

/* Resolve the USB device sysfs dir for composite sibling open. Prefer the
 * stable bus id (Flydigi dongle) over VID/PID scan (ambiguous with duplicates). */
static int resolve_usbdir_for_controller(ctm_controller_t *c, char *out, size_t out_len)
{
    if (c && c->dev.usb_busid[0] &&
        composite_usb_device_dir_by_busid(c->dev.usb_busid, out, out_len) == 0) {
        return 0;
    }
    return resolve_usb_device_dir(c->vid_num, c->pid_num, out, out_len);
}

/* Open every class-03 HID interface of the puck's USB device R/W (the siblings),
 * and record the primary's (c->dev.path) endpoints + interface number -- all via
 * the reliable /sys/class/input resolution (no flaky /sys/class/hidraw realpath).
 * The primary is NOT added to comp[]; its endpoints go to c->primary_*.
 * When: run_session start, for composite controllers (the puck). */
static void open_composite_siblings(ctm_controller_t *c)
{
    if (c && c->flydigi_xinput_evdev_only) {
        return;
    }
    char usbdir[512];
    if (resolve_usbdir_for_controller(c, usbdir, sizeof(usbdir)) != 0) {
        ctl_log(c, "composite: USB device dir unresolved vid=%04x pid=%04x busid=%s",
                c->vid_num, c->pid_num, c->dev.usb_busid[0] ? c->dev.usb_busid : "-");
        return;
    }
    const char *base = strrchr(usbdir, '/'); base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    DIR *d = opendir(usbdir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && c->comp_count < 15) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[1024], clsf[1100], cls[8] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(clsf, sizeof(clsf), "%s/bInterfaceClass", ifdir);
        if (read_text_file(clsf, cls, sizeof(cls)) != 0 || strcmp(cls, "03") != 0) continue;
        char hidpath[64];
        if (find_hidraw_under(ifdir, hidpath, sizeof(hidpath)) != 0) continue;
        uint8_t in_ep = 0, out_ep = 0, iface = 0xff;
        char numf[1100], num[8] = {0};
        snprintf(numf, sizeof(numf), "%s/bInterfaceNumber", ifdir);
        if (read_text_file(numf, num, sizeof(num)) == 0) iface = (uint8_t)strtoul(num, NULL, 16);
        iface_endpoints(ifdir, &in_ep, &out_ep);
        if (strcmp(hidpath, c->dev.path) == 0) {            /* the primary */
            c->primary_in_ep = in_ep; c->primary_out_ep = out_ep; c->primary_iface = iface;
            ctl_log(c, "composite primary %s if=%u in_ep=0x%02x out_ep=0x%02x",
                    hidpath, iface, in_ep, out_ep);
            continue;
        }
        int fd = open(hidpath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) fd = open(hidpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        comp_iface_t *ci = &c->comp[c->comp_count++];
        ci->c = c; ci->fd = fd; ci->started = 0;
        ci->in_ep = in_ep; ci->out_ep = out_ep; ci->iface = iface;
        ctl_log(c, "composite sibling %s if=%u in_ep=0x%02x out_ep=0x%02x",
                hidpath, iface, in_ep, out_ep);
    }
    closedir(d);
}

typedef struct {
    uint16_t buttons;
    int8_t hat_x;
    int8_t hat_y;
    uint8_t lt;
    uint8_t rt;
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
} xpad_evdev_state_t;

static void xpad_sync_hat_buttons(xpad_evdev_state_t *st)
{
    st->buttons &= (uint16_t)~0x000fu;
    if (st->hat_x < 0) st->buttons |= 0x0004u;
    else if (st->hat_x > 0) st->buttons |= 0x0008u;
    if (st->hat_y < 0) st->buttons |= 0x0001u;
    else if (st->hat_y > 0) st->buttons |= 0x0002u;
}

static void xpad_apply_hat(xpad_evdev_state_t *st, uint16_t code, int value)
{
    if (code == ABS_HAT0X) {
        st->hat_x = (int8_t)value;
    } else if (code == ABS_HAT0Y) {
        st->hat_y = (int8_t)value;
    } else {
        return;
    }
    xpad_sync_hat_buttons(st);
}

static void xpad_map_button(xpad_evdev_state_t *st, uint16_t code, int value)
{
    uint16_t mask = 0;
    switch (code) {
    case BTN_SOUTH: mask = 0x1000; break;
    case BTN_EAST: mask = 0x2000; break;
    case BTN_WEST: mask = 0x8000; break;
    case BTN_NORTH: mask = 0x4000; break;
    case BTN_TL: mask = 0x0100; break;
    case BTN_TR: mask = 0x0200; break;
    case BTN_SELECT: mask = 0x0020; break;
    case BTN_START: mask = 0x0010; break;
    case BTN_MODE: mask = 0x0400; break;
    case BTN_THUMBL: mask = 0x0040; break;
    case BTN_THUMBR: mask = 0x0080; break;
    default:
        if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT) {
            static const uint16_t dpad_map[] = {0x0001, 0x0002, 0x0004, 0x0008};
            st->hat_x = 0;
            st->hat_y = 0;
            st->buttons &= (uint16_t)~0x000fu;
            if (value) st->buttons |= dpad_map[code - BTN_DPAD_UP];
            return;
        }
        break;
    }
    if (!mask) return;
    if (value) st->buttons |= mask;
    else st->buttons &= (uint16_t)~mask;
}

static void xpad_build_hid_report(const xpad_evdev_state_t *st, uint8_t *buf)
{
    memset(buf, 0, 20);
    buf[0] = 0x00;
    buf[1] = 0x14;
    buf[2] = (uint8_t)(st->buttons & 0xffu);
    buf[3] = (uint8_t)((st->buttons >> 8) & 0xffu);
    buf[4] = st->lt;
    buf[5] = st->rt;
    buf[6] = (uint8_t)(st->lx & 0xff);
    buf[7] = (uint8_t)((st->lx >> 8) & 0xff);
    buf[8] = (uint8_t)(st->ly & 0xff);
    buf[9] = (uint8_t)((st->ly >> 8) & 0xff);
    buf[10] = (uint8_t)(st->rx & 0xff);
    buf[11] = (uint8_t)((st->rx >> 8) & 0xff);
    buf[12] = (uint8_t)(st->ry & 0xff);
    buf[13] = (uint8_t)((st->ry >> 8) & 0xff);
}

static bool iface_is_xpad_gamepad_ff(const char *ifdir)
{
    char cls[8] = {0}, sub[8] = {0}, proto[8] = {0};
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/bInterfaceClass", ifdir);
    read_text_file(path, cls, sizeof(cls));
    snprintf(path, sizeof(path), "%s/bInterfaceSubClass", ifdir);
    read_text_file(path, sub, sizeof(sub));
    snprintf(path, sizeof(path), "%s/bInterfaceProtocol", ifdir);
    read_text_file(path, proto, sizeof(proto));
    return strcmp(cls, "ff") == 0 && strcmp(sub, "5d") == 0 && strtoul(proto, NULL, 16) == 0x01u;
}

static int find_xpad_iface_endpoints(ctm_controller_t *c, uint8_t *in_ep, uint8_t *out_ep)
{
    if (in_ep) *in_ep = 0;
    if (out_ep) *out_ep = 0;
    char usbdir[512];
    if (resolve_usbdir_for_controller(c, usbdir, sizeof(usbdir)) != 0) return -1;
    const char *base = strrchr(usbdir, '/');
    base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    DIR *d = opendir(usbdir);
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    uint8_t fallback_in = 0;
    uint8_t fallback_out = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[1024], clsf[1100], cls[8] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(clsf, sizeof(clsf), "%s/bInterfaceClass", ifdir);
        if (read_text_file(clsf, cls, sizeof(cls)) != 0) {
            continue;
        }
        const bool is_hid = strcmp(cls, "03") == 0;
        const bool is_xpad_ff = iface_is_xpad_gamepad_ff(ifdir);
        if (!is_hid && !is_xpad_ff) {
            continue;
        }
        char hidpath[64];
        if (is_hid && find_hidraw_under(ifdir, hidpath, sizeof(hidpath)) == 0) {
            continue;
        }
        uint8_t in = 0, out = 0;
        iface_endpoints(ifdir, &in, &out);
        if (in == 0) {
            continue;
        }
        if (is_xpad_ff) {
            if (in_ep) *in_ep = in;
            if (out_ep) *out_ep = out;
            rc = 0;
            break;
        }
        if (fallback_in == 0) {
            fallback_in = in;
            fallback_out = out;
        }
    }
    closedir(d);
    if (rc != 0 && fallback_in != 0) {
        if (in_ep) *in_ep = fallback_in;
        if (out_ep) *out_ep = fallback_out;
        rc = 0;
    }
    return rc;
}

static int find_xpad_iface_endpoint(ctm_controller_t *c, uint8_t *in_ep)
{
    return find_xpad_iface_endpoints(c, in_ep, NULL);
}

static void xpad_stop_rumble(ctm_controller_t *c)
{
    if (!c || c->evdev_gamepad_fd < 0 || c->xpad_ff_effect_id < 0) {
        return;
    }
    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (uint16_t)c->xpad_ff_effect_id;
    play.value = 0;
    (void)write(c->evdev_gamepad_fd, &play, sizeof(play));
}

/* Parse Xbox 360 wired motor bytes from a host OUT report. Returns 0 only for
 * rumble payloads (00 08 [strong] [weak] ...). Short vendor handshakes on ep
 * 0x05 (e.g. 01 03 02) are ignored so they are not mistaken for motors. */
static int xpad_parse_rumble_motors(const uint8_t *payload, size_t len,
                                    uint8_t *strong_out, uint8_t *weak_out)
{
    if (!payload || !strong_out || !weak_out || len < 4) {
        return -1;
    }
    if (payload[0] == 0x01) {
        return -1;
    }
    if (payload[0] != 0x00 || payload[1] != 0x08) {
        return -1;
    }
    *strong_out = payload[2];
    *weak_out = payload[3];
    return 0;
}

static void xpad_apply_rumble(ctm_controller_t *c, const uint8_t *payload, size_t len)
{
    if (!c || c->evdev_gamepad_fd < 0 || !payload) {
        return;
    }
    uint8_t weak = 0;
    uint8_t strong = 0;
    if (xpad_parse_rumble_motors(payload, len, &strong, &weak) != 0) {
        return;
    }

    struct ff_effect eff;
    memset(&eff, 0, sizeof(eff));
    eff.type = FF_RUMBLE;
    eff.id = c->xpad_ff_effect_id;
    eff.u.rumble.weak_magnitude = (uint16_t)((unsigned)weak * 0xffffu / 255u);
    eff.u.rumble.strong_magnitude = (uint16_t)((unsigned)strong * 0xffffu / 255u);
    if (ioctl(c->evdev_gamepad_fd, EVIOCSFF, &eff) < 0) {
        return;
    }
    if (c->xpad_ff_effect_id < 0) {
        c->xpad_ff_effect_id = eff.id;
    }

    if (!weak && !strong) {
        xpad_stop_rumble(c);
        return;
    }

    struct input_event play;
    memset(&play, 0, sizeof(play));
    gettimeofday(&play.time, NULL);
    play.type = EV_FF;
    play.code = (uint16_t)eff.id;
    play.value = 1;
    (void)write(c->evdev_gamepad_fd, &play, sizeof(play));
}

static int open_xpad_evdev_for_busid(const char *usb_busid, char *path_out, size_t path_len)
{
    if (!usb_busid || !usb_busid[0] || !path_out || path_len == 0) return -1;
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *ent;
    int rc = -1;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char input_path[PATH_MAX], busid[64] = {0};
        snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", ent->d_name);
        usb_busid_from_input_path(input_path, busid, sizeof(busid));
        if (strcmp(busid, usb_busid) != 0) continue;

        char vendor[16] = {0}, product[16] = {0}, name[128] = {0};
        char attr[PATH_MAX];
        snprintf(attr, sizeof(attr), "%s/id/vendor", input_path);
        read_text_file(attr, vendor, sizeof(vendor));
        snprintf(attr, sizeof(attr), "%s/id/product", input_path);
        read_text_file(attr, product, sizeof(product));
        snprintf(attr, sizeof(attr), "%s/name", input_path);
        read_text_file(attr, name, sizeof(name));
        if (!hex_equals(vendor, 0x045e) || !hex_equals(product, 0x028e)) {
            if (!contains_ci(name, "x-box") && !contains_ci(name, "xbox")) continue;
        }

        DIR *input = opendir(input_path);
        if (!input) continue;
        struct dirent *child;
        while ((child = readdir(input)) != NULL) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            snprintf(path_out, path_len, "/dev/input/%s", child->d_name);
            rc = 0;
            break;
        }
        closedir(input);
        if (rc == 0) break;
    }
    closedir(d);
    return rc;
}

static void xpad_send_report(ctm_controller_t *c, const xpad_evdev_state_t *st)
{
    if (!c || c->xpad_in_ep == 0) return;
    uint8_t buf[20];
    xpad_build_hid_report(st, buf);
    if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->xpad_in_ep, buf, sizeof(buf)) == 0) {
        c->st_reports_in++;
    }
}

static void *evdev_gamepad_thread_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    xpad_evdev_state_t st;
    memset(&st, 0, sizeof(st));
    xpad_send_report(c, &st);

    while (!c->stop && c->evdev_gamepad_fd >= 0) {
        struct pollfd pfd;
        pfd.fd = c->evdev_gamepad_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;
        for (;;) {
            struct input_event ev;
            ssize_t n = read(c->evdev_gamepad_fd, &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) break;
            if (ev.type == EV_KEY) {
                xpad_map_button(&st, ev.code, ev.value);
                xpad_send_report(c, &st);
            } else if (ev.type == EV_ABS) {
                switch (ev.code) {
                case ABS_X: st.lx = (int16_t)ev.value; break;
                case ABS_Y: st.ly = (int16_t)(-(int)ev.value); break;
                case ABS_RX: st.rx = (int16_t)ev.value; break;
                case ABS_RY: st.ry = (int16_t)(-(int)ev.value); break;
                case ABS_Z: st.lt = (uint8_t)(ev.value > 255 ? ev.value >> 7 : ev.value); break;
                case ABS_RZ: st.rt = (uint8_t)(ev.value > 255 ? ev.value >> 7 : ev.value); break;
                case ABS_HAT0X:
                case ABS_HAT0Y:
                    xpad_apply_hat(&st, ev.code, ev.value);
                    break;
                default: continue;
                }
                xpad_send_report(c, &st);
            }
        }
    }
    return NULL;
}

static int start_evdev_gamepad_feeder(ctm_controller_t *c)
{
    if (!c || !(c->ops && c->ops->composite_evdev_gamepad) || !c->dev.usb_busid[0]) {
        return -1;
    }
    if (find_xpad_iface_endpoints(c, &c->xpad_in_ep, &c->xpad_out_ep) != 0 || c->xpad_in_ep == 0) {
        ctl_log(c, "xpad interface endpoint not found busid=%s", c->dev.usb_busid);
        return -1;
    }

    char evdev_path[64];
    if (open_xpad_evdev_for_busid(c->dev.usb_busid, evdev_path, sizeof(evdev_path)) != 0) {
        ctl_log(c, "xpad evdev not found busid=%s", c->dev.usb_busid);
        return -1;
    }

    int fd = open(evdev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ctl_log(c, "xpad evdev open failed path=%s errno=%d", evdev_path, errno);
        return -1;
    }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        ctl_log(c, "xpad evdev grab failed path=%s errno=%d (continuing)", evdev_path, errno);
    }
    c->xpad_ff_effect_id = -1;
    c->evdev_gamepad_fd = fd;
    if (pthread_create(&c->evdev_gamepad_thread, NULL, evdev_gamepad_thread_main, c) == 0) {
        c->evdev_gamepad_started = 1;
        ctl_log(c, "xpad evdev feeder started path=%s in_ep=0x%02x out_ep=0x%02x",
                evdev_path, c->xpad_in_ep, c->xpad_out_ep);
        return 0;
    }
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    c->evdev_gamepad_fd = -1;
    return -1;
}

static void stop_evdev_gamepad_feeder(ctm_controller_t *c)
{
    if (!c) return;
    if (c->evdev_gamepad_started) {
        pthread_join(c->evdev_gamepad_thread, NULL);
        c->evdev_gamepad_started = 0;
    }
    if (c->evdev_gamepad_fd >= 0) {
        xpad_stop_rumble(c);
        ioctl(c->evdev_gamepad_fd, EVIOCGRAB, 0);
        close(c->evdev_gamepad_fd);
        c->evdev_gamepad_fd = -1;
        c->xpad_ff_effect_id = -1;
    }
}

/* Sibling reader: poll one interface's hidraw, forward input tagged with its IN
 * endpoint. When: one thread per sibling, while comp_run. */
static void *composite_reader_main(void *arg)
{
    comp_iface_t *ci = (comp_iface_t *)arg;
    ctm_controller_t *c = ci->c;
    while (c->comp_run && !c->stop) {
        struct pollfd pfd; pfd.fd = ci->fd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfd.revents & POLLIN)) continue;
        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n = read(ci->fd, buf, sizeof(buf));
            if (n > 0) {
                if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, ci->in_ep, buf, (size_t)n) != 0) break;
                c->st_reports_in++;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }
    return NULL;
}

/* Per-burst input coalescing. The WiFi+BT combo chip starves BT scheduling under
 * heavy stream RX, so the DS5 buffers reports across a ~16-28ms stall then flushes
 * them back-to-back. Forwarding every stale report FIFO over usbip backlogs the
 * game's HID input queue and adds felt latency; SDL/Moonlight only ever consumed
 * the latest gamepad state, which is why the SDL path felt smoother. Match that:
 * within ONE readable burst keep only the most recent report per report-id, then
 * forward. In steady state a poll yields a single report, so this is a no-op. */
#define CTM_COAL_MAX_IDS 6
static void *input_thread_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    ctl_set_rt_prio(c, "ctm-input", 12);
    /* Allocated once (MAX_REPORT is 4096); per-thread so concurrent controllers
     * don't share. */
    static __thread uint8_t coal_buf[CTM_COAL_MAX_IDS][MAX_REPORT];
    size_t  coal_len[CTM_COAL_MAX_IDS];
    uint8_t coal_id[CTM_COAL_MAX_IDS];
    while (!c->stop) {
        struct pollfd pfds[2];
        pfds[0].fd = c->hid_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = c->wake_pipe[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
        int pr = poll(pfds, 2, 1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfds[1].revents & POLLIN) break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfds[0].revents & POLLIN)) continue;
        int coal_n = 0, drained = 0;
        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n = read(c->hid_fd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                break;
            }
            uint8_t id = buf[0];
            int slot = -1;
            for (int i = 0; i < coal_n; ++i) if (coal_id[i] == id) { slot = i; break; }
            if (slot < 0) {
                slot = (coal_n < CTM_COAL_MAX_IDS) ? coal_n++ : (CTM_COAL_MAX_IDS - 1);
                coal_id[slot] = id;
            }
            memcpy(coal_buf[slot], buf, (size_t)n);
            coal_len[slot] = (size_t)n;
            drained++;
        }
        for (int i = 0; i < coal_n && !c->stop; ++i) {
            if (c_send(c, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, c->primary_in_ep,
                       coal_buf[i], coal_len[i]) != 0) {
                c->stop = 1;
                break;
            }
            c->st_reports_in++;
        }
        if (drained > coal_n) c->st_coalesced += (unsigned long)(drained - coal_n);
    }
    return NULL;
}

/* Send HELLO (caps + HID report descriptor). When: first message of each
 * session, from handshake. */
static int send_hello(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                      const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_hid_descriptor_info_t desc_info;
    memset(&desc_info, 0, sizeof(desc_info));
    desc_info.report_descriptor_len = report_desc_len;
    size_t hello_len = sizeof(*caps) + sizeof(desc_info) + report_desc_len;
    uint8_t *hello = (uint8_t *)malloc(hello_len);
    if (!hello) return -1;
    memcpy(hello, caps, sizeof(*caps));
    memcpy(hello + sizeof(*caps), &desc_info, sizeof(desc_info));
    if (report_desc_len) {
        memcpy(hello + sizeof(*caps) + sizeof(desc_info), report_desc, report_desc_len);
    }
    int rc = c_send(c, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, hello_len);
    free(hello);
    return rc;
}

/* Composite: pick the hidraw fd for a feature request by the interface number
 * encoded in the high byte of request_id (set by the host). Non-composite, or no
 * matching interface, falls back to the primary fd. */
static int feature_fd_for(ctm_controller_t *c, uint32_t request_id)
{
    if (!(c->ops && c->ops->composite)) return c->hid_fd;
    uint8_t iface = (uint8_t)(request_id >> 24);
    if (iface == c->primary_iface) return c->hid_fd;
    for (int i = 0; i < c->comp_count; ++i) {
        if (c->comp[i].iface == iface) return c->comp[i].fd;
    }
    return c->hid_fd;
}

/* Dispatch one inbound message: OUTPUT (paced or direct write), FEATURE_GET/SET
 * (hidraw ioctl + reply), HOST_CONFIG (pacing params). When: per message
 * decoded in the session loop. */
static void handle_message(ctm_controller_t *c, ctmb_host_config_t *host_cfg,
                           queued_report_t *paced_q, int *paced_head, int *paced_count,
                           const ctmb_header_t *h, uint8_t *payload)
{
    if (h->type == CTMB_MSG_OUTPUT_REPORT) {
        if (c->ops && c->ops->composite_evdev_gamepad && c->evdev_gamepad_fd >= 0) {
            uint8_t ep = (uint8_t)h->request_id;
            if (ep == 0 || ep == c->xpad_out_ep || ep == c->primary_out_ep) {
                xpad_apply_rumble(c, payload, h->payload_len);
                c->st_reports_out++;
                return;
            }
        }
        if (c->ops && c->ops->composite) {
            /* Route the host's OUT write to the interface that owns the OUT
             * endpoint it addressed (request_id = endpoint). Verbatim; pacing is
             * a BT-audio concern that does not apply to the composite puck. */
            int fd = c->hid_fd;
            uint8_t ep = (uint8_t)h->request_id;
            if (ep != 0 && ep != c->primary_out_ep) {
                for (int i = 0; i < c->comp_count; ++i) {
                    if (c->comp[i].out_ep == ep) { fd = c->comp[i].fd; break; }
                }
            }
            (void)hid_write_fd_raw(c, fd, payload, h->payload_len);
        } else if (should_pace(host_cfg, h, payload, h->payload_len)) {
            queue_paced(paced_q, paced_head, paced_count, payload, h->payload_len);
        } else {
            (void)hid_write_report(c, payload, h->payload_len);
        }
    } else if (h->type == CTMB_MSG_FEATURE_GET) {
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            int fd = feature_fd_for(c, h->request_id);
            pthread_mutex_lock(&c->hid_mutex);
            int rc = ioctl(fd, HIDIOCGFEATURE(h->payload_len), feature);
            pthread_mutex_unlock(&c->hid_mutex);
            if (rc >= 0) {
                ok = c_send(c, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK,
                            h->request_id, feature, h->payload_len) == 0;
            }
        }
        if (!ok) (void)c_send(c, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0);
    } else if (h->type == CTMB_MSG_FEATURE_SET) {
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            int fd = feature_fd_for(c, h->request_id);
            pthread_mutex_lock(&c->hid_mutex);
            ok = ioctl(fd, HIDIOCSFEATURE(h->payload_len), feature) >= 0;
            pthread_mutex_unlock(&c->hid_mutex);
        }
        (void)c_send(c, CTMB_MSG_FEATURE_REPORT, ok ? CTMB_FLAG_OK : 0, h->request_id, NULL, 0);
    } else if (h->type == CTMB_MSG_HOST_CONFIG && h->payload_len >= sizeof(*host_cfg)) {
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    }
}

static int handshake(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                     const uint8_t *report_desc, uint32_t report_desc_len,
                     ctmb_host_config_t *host_cfg)
{
    /* Composite (puck): forward the captured enumeration verbatim BEFORE HELLO so
     * the host can build the composite device from it. Identity passthrough. */
    if (c->enum_payload && c->enum_payload_len > 0 &&
        c_send(c, CTMB_MSG_ENUM, CTMB_FLAG_OK, 0, c->enum_payload, (size_t)c->enum_payload_len) != 0) {
        ctl_log(c, "ENUM send failed");
        return -1;
    }
    if (send_hello(c, caps, report_desc, report_desc_len) != 0) {
        ctl_log(c, "HELLO failed");
        return -1;
    }
    /* Relay types (puck/xbox/generic) do not require HOST_CONFIG — proceed
     * straight to the loop, which still applies HOST_CONFIG if it arrives. */
    if (!c->ops->needs_host_config) {
        host_cfg->bt_pace_us = 10667;
        return 0;
    }
    ctmb_header_t h;
    uint8_t *payload = NULL;
    uint64_t start = now_us();
    for (;;) {
        if (c->stop) return -1;
        if (c->xport.kind == CTM_TRANSPORT_ENET) {
            if (ctm_transport_service(&c->xport, 50) < 0) { ctl_log(c, "host config wait: link dropped"); return -1; }
        }
        int got = c_recv(c, &h, &payload);
        if (got < 0) { ctl_log(c, "host config receive failed"); return -1; }
        if (got == 0) {
            if (now_us() - start >= 5000000ull) { ctl_log(c, "host config timeout"); return -1; }
            continue;
        }
        if (h.type != CTMB_MSG_HOST_CONFIG || h.payload_len < sizeof(*host_cfg)) {
            ctl_log(c, "host config unexpected type=%u len=%u", h.type, h.payload_len);
            free(payload);
            return -1;
        }
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        free(payload);
        break;
    }
    if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    return 0;
}

/* Run one connected session: handshake, start the reader thread, then the
 * output/feature receive loop + paced drain until the link drops or stop. Tears
 * the reader thread down on exit. When: per successful connect, from session_main. */
static void run_session(ctm_controller_t *c, const ctmb_device_caps_t *caps,
                        const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_host_config_t host_cfg;
    queued_report_t paced_q[PACED_QUEUE_CAP];
    int paced_head = 0, paced_count = 0;
    uint64_t next_paced_us = 0;
    memset(&host_cfg, 0, sizeof(host_cfg));
    memset(paced_q, 0, sizeof(paced_q));

    if (handshake(c, caps, report_desc, report_desc_len, &host_cfg) != 0) return;

    { uint8_t drain[64]; while (read(c->wake_pipe[0], drain, sizeof(drain)) > 0) { /* discard */ } }

    /* Composite: open siblings + resolve the primary endpoints BEFORE the input
     * thread starts (it tags primary input with c->primary_in_ep). */
    if (c->ops->composite) {
        open_composite_siblings(c);
    }
    if (c->ops->composite_evdev_gamepad) {
        if (start_evdev_gamepad_feeder(c) != 0 &&
            c->ops->kind && strcmp(c->ops->kind, "flydigi") == 0 &&
            (flydigi_is_xinput_evdev_only_for_busid(c->dev.usb_busid) ||
             flydigi_is_xinput_mode_for_busid(c->dev.usb_busid))) {
            ctl_log(c, "xpad evdev feeder failed; aborting xinput session");
            return;
        }
    }

    if (c->flydigi_xinput_evdev_only) {
        c->input_thread_started = 0;
    } else if (pthread_create(&c->input_thread, NULL, input_thread_main, c) == 0) {
        c->input_thread_started = 1;
    } else {
        ctl_log(c, "input thread failed errno=%d", errno);
        for (int i = 0; i < c->comp_count; ++i)
            if (c->comp[i].fd >= 0) { close(c->comp[i].fd); c->comp[i].fd = -1; }
        c->comp_count = 0;
        return;
    }
    c->comp_run = 1;
    if (c->ops->composite && !c->flydigi_xinput_evdev_only) {
        for (int i = 0; i < c->comp_count; ++i) {
            if (pthread_create(&c->comp[i].thread, NULL, composite_reader_main, &c->comp[i]) == 0)
                c->comp[i].started = 1;
        }
        if (c->comp_count > 0)
            ctl_log(c, "composite: forwarding %d sibling interface(s)", c->comp_count);
    }
    ctl_log(c, "active host=%s port=%d path=%s product=%s transport=%s",
         c->host, c->port, c->dev.path, caps->product,
         c->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 1;
    c->st_transport_enet = (c->xport.kind == CTM_TRANSPORT_ENET) ? 1 : 0;
    pthread_mutex_unlock(&c->status_mutex);

    int link_alive = 1;
    while (!c->stop && link_alive) {
        /* The ~10.6ms host pace (~94/s) was sized for the BT one-outstanding wall
         * on the hidraw write path. The raw-ACL injector bypasses that wall, but at
         * ~94/s the pace barely lags the ~100/s DS5 audio source, so the paced queue
         * parks near-full (up to PACED_QUEUE_CAP*pace ~= 340ms of feedback latency).
         * While the forwarder is actively injecting, tighten the pace to <=8ms so the
         * queue drains with margin and stays shallow; the hidraw fallback keeps the
         * conservative host pace. */
        uint32_t eff_pace_us = host_cfg.bt_pace_us;
        if (c->acl_tx) {
            long ij_ = 0, dp_ = 0;
            int rdy_ = 0;
            ds5_acl_tx_stats(c->acl_tx, &ij_, &dp_, &rdy_);
            if (rdy_ && eff_pace_us > 8000) eff_pace_us = 8000;
        }
        drain_paced(c, paced_q, &paced_head, &paced_count, &next_paced_us, eff_pace_us);
        if (c->plc_enabled) {
            uint64_t pnow = now_us();
            if (c->plc_log_next_us == 0) {
                c->plc_log_next_us = pnow + 60000000ull;
            } else if (pnow >= c->plc_log_next_us) {
                long inj = 0, drp = 0;
                int rdy = 0;
                if (c->acl_tx) {
                    ds5_acl_tx_stats(c->acl_tx, &inj, &drp, &rdy);
                }
                ctl_log(c, "PLC/60s: audio_omit=%lu conceal=%lu capdrop=%lu | acl_ready=%d inj=%ld drop=%ld | hid ok=%lu eagain=%lu recov=%lu drop=%lu | dedup31=%lu | in=%lu coal=%lu",
                        c->st_audio_omit, c->st_audio_conceal, c->st_audio_capdrop, rdy, inj, drp,
                        c->st_hid_ok, c->st_hid_eagain, c->st_hid_recovered, c->st_hid_dropped,
                        c->st_dedup_skipped, c->st_reports_in, c->st_coalesced);
                c->st_audio_omit = c->st_audio_conceal = c->st_audio_capdrop = 0;
                c->st_hid_ok = c->st_hid_eagain = c->st_hid_recovered = c->st_hid_dropped = 0;
                c->st_dedup_skipped = 0;
                c->plc_log_next_us = pnow + 60000000ull;
            }
        }
        int timeout_ms = 50;
        if (paced_count > 0 && next_paced_us != 0) {
            uint64_t now = now_us();
            timeout_ms = next_paced_us <= now ? 0 : (int)((next_paced_us - now) / 1000u);
            if (timeout_ms > 50) timeout_ms = 50;
        }
        if (c->xport.kind == CTM_TRANSPORT_ENET) {
            if (ctm_transport_service(&c->xport, 1) < 0) {
                ctl_log(c, "ENet link lost");
                link_alive = 0;
            } else {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                while (c_recv(c, &h, &payload) == 1) {
                    handle_message(c, &host_cfg, paced_q, &paced_head, &paced_count, &h, payload);
                    free(payload);
                    payload = NULL;
                }
            }
        } else {
            struct pollfd pfd;
            pfd.fd = c->xport.fd; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                link_alive = 0;
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                link_alive = 0;
            } else if (pfd.revents & POLLIN) {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                if (c_recv(c, &h, &payload) != 1) {
                    free(payload);
                    link_alive = 0;
                } else {
                    handle_message(c, &host_cfg, paced_q, &paced_head, &paced_count, &h, payload);
                    free(payload);
                }
            }
        }
    }

    pthread_mutex_lock(&c->status_mutex);
    c->st_connected = 0;
    pthread_mutex_unlock(&c->status_mutex);

    c->comp_run = 0;
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    stop_evdev_gamepad_feeder(c);
    if (c->input_thread_started) {
        pthread_join(c->input_thread, NULL);
        c->input_thread_started = 0;
    }
    for (int i = 0; i < c->comp_count; ++i) {
        if (c->comp[i].started) pthread_join(c->comp[i].thread, NULL);
        if (c->comp[i].fd >= 0) { close(c->comp[i].fd); c->comp[i].fd = -1; }
    }
    c->comp_count = 0;
}

/* Session thread body: open HID + wake pipe once, then the dual-probe
 * connect -> grab -> run_session -> release -> disconnect loop until plug_out.
 * When: the controller's own thread, started by plug_in. */
static void *session_main(void *arg)
{
    ctm_controller_t *c = (ctm_controller_t *)arg;
    ctl_set_rt_prio(c, "ctm-session", 15);
    ctmb_device_caps_t caps;
    uint8_t report_desc[MAX_REPORT_DESCRIPTOR];
    uint32_t report_desc_len = 0;

    if (c->ops && c->ops->raw_acl_output) {
        const char *env = getenv("CTM_RAW_ACL");
        if (!env || strcmp(env, "0") != 0) {
            c->acl_tx = ds5_acl_tx_start(0, acl_log_cb, c);
            ctl_log(c, "raw-ACL output %s", c->acl_tx ? "enabled" : "unavailable (hidraw)");
        }
        const char *penv = getenv("CTM_AUDIO_PLC");
        c->plc_enabled = (!penv || strcmp(penv, "0") != 0);
        ctl_log(c, "audio PLC %s", c->plc_enabled ? "enabled" : "disabled");
        const char *wenv = getenv("CTM_HID_WAIT_MS");
        c->hid_wait_ms = wenv ? atoi(wenv) : 3;
        if (c->hid_wait_ms < 0) {
            c->hid_wait_ms = 0;
        }
        if (c->hid_wait_ms > 20) {
            c->hid_wait_ms = 20;
        }
        ctl_log(c, "hid EAGAIN wait=%dms", c->hid_wait_ms);
        const char *denv = getenv("CTM_DEDUP");
        c->dedup_enabled = (!denv || strcmp(denv, "0") != 0);
        ctl_log(c, "0x31 dedup %s", c->dedup_enabled ? "enabled" : "disabled");
    }

    c->hid_fd = open_hid(c, &caps, report_desc, &report_desc_len);
    if (c->hid_fd < 0 && c->ops && strcmp(c->ops->kind, "flydigi") == 0 &&
        flydigi_is_xinput_evdev_only_for_busid(c->dev.usb_busid)) {
        c->hid_fd = flydigi_open_xinput_evdev_only(c, &caps, report_desc, &report_desc_len);
    }
    if (c->hid_fd < 0 && c->ops && strcmp(c->ops->kind, "flydigi") == 0) {
        c->hid_fd = flydigi_open_xinput_handshake(c, &caps, report_desc, &report_desc_len);
    }
    if (c->hid_fd < 0) {
        ctl_log(c, "hid open failed path=%s errno=%d", c->dev.path, errno);
        c->stop = 1;
        return NULL;
    }
    /* Composite primary endpoints are resolved in open_composite_siblings()
     * (reliable /sys/class/input path), at run_session start. */
    if (pipe(c->wake_pipe) != 0) {
        ctl_log(c, "wake pipe failed errno=%d", errno);
        if (c->acl_tx) {
            ds5_acl_tx_stop(c->acl_tx);
            c->acl_tx = NULL;
        }
        c->stop = 1;
        return NULL;
    }
    (void)fcntl(c->wake_pipe[0], F_SETFL, fcntl(c->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(c->wake_pipe[1], F_SETFL, fcntl(c->wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    while (!c->stop) {
        while (!c->stop &&
               ctm_transport_connect_once(&c->xport, c->host, c->port, 400) != 0) {
            for (int slept = 0; slept < 500 && !c->stop; slept += 50) usleep(50000);
        }
        if (c->stop) break;
        ctl_log(c, "connected via %s", c->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");

        if (c->ops->grab_evdev) grab_matching_evdev(c);
        if (c->ops->on_plug_init) c->ops->on_plug_init(c, &c->xport);
        run_session(c, &caps, report_desc, report_desc_len);
        release_evdev_grabs(c);

        ctm_transport_disconnect(&c->xport);
        if (!c->stop) ctl_log(c, "link lost; retrying probe loop");
    }
    if (c->acl_tx) {
        long inj = 0, drp = 0;
        ds5_acl_tx_stats(c->acl_tx, &inj, &drp, NULL);
        ctl_log(c, "raw-ACL output: injected=%ld dropped=%ld", inj, drp);
        ds5_acl_tx_stop(c->acl_tx);
        c->acl_tx = NULL;
    }
    c->stop = 1;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

/* Build an idle controller for a detected device (factory picks its ops).
 * When: the UI/monitor decides to offer a device; before plug_in. */
ctm_controller_t *ctm_controller_create(const ctm_controller_dev_t *dev)
{
    if (!dev) return NULL;
    ctm_controller_t *c = (ctm_controller_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->dev = *dev;
    c->ops = ctm_controller_ops_for(dev);
    c->vid_num = (unsigned int)strtoul(dev->vid, NULL, 16);
    c->pid_num = (unsigned int)strtoul(dev->pid, NULL, 16);
    c->hid_fd = -1;
    c->wake_pipe[0] = -1;
    c->wake_pipe[1] = -1;
    c->evdev_gamepad_fd = -1;
    c->xpad_ff_effect_id = -1;
    c->flydigi_xinput_evdev_only = 0;
    c->dummy_hid_pipe_wr = -1;
    for (int i = 0; i < MAX_EVDEV_GRABS; ++i) c->evdev_grabs[i].fd = -1;
    pthread_mutex_init(&c->hid_mutex, NULL);
    pthread_mutex_init(&c->settings_mutex, NULL);
    pthread_mutex_init(&c->status_mutex, NULL);
    return c;
}

/* Open this controller's per-MAC log file (/tmp/ctm-<mac>.log). When: plug_in. */
static void open_log(ctm_controller_t *c)
{
    char mac[64], name[128];
    size_t o = 0;
    for (size_t i = 0; c->dev.mac[i] && o + 1 < sizeof(mac); ++i) {
        char ch = c->dev.mac[i];
        if (ch != ':') mac[o++] = ch;
    }
    mac[o] = '\0';
    snprintf(name, sizeof(name), "/tmp/ctm-%s.log", mac[0] ? mac : c->ops->kind);
    c->log = fopen(name, "a");
}

/* Start bridging: open log, bring up ENet + transport, launch the session
 * thread. Returns once the thread is started (connect happens async). When: the
 * user clicks Plug in. */
/* Composite: hand the controller the forwarded enumeration payload (CTMB_MSG_ENUM),
 * which handshake sends verbatim before HELLO. Copies the bytes. When: app plug-in. */
void ctm_controller_set_enum_payload(ctm_controller_t *c, const uint8_t *payload, int len)
{
    if (!c) return;
    free(c->enum_payload);
    c->enum_payload = NULL;
    c->enum_payload_len = 0;
    if (payload && len > 0) {
        c->enum_payload = (uint8_t *)malloc((size_t)len);
        if (c->enum_payload) {
            memcpy(c->enum_payload, payload, (size_t)len);
            c->enum_payload_len = len;
        }
    }
}

int ctm_controller_plug_in(ctm_controller_t *c, const char *host, int port)
{
    if (!c || !host || !host[0] || port <= 0) return -1;
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->stop = 0;
    open_log(c);

    pthread_once(&g_enet_once, enet_global_init_once);
    if (g_enet_ready) {
        c->enet = enet_client_create();
        if (!c->enet) ctl_log(c, "enet client create failed; ENet disabled");
    }
    ctm_transport_init(&c->xport, c->enet);

    if (pthread_create(&c->session_thread, NULL, session_main, c) != 0) {
        ctl_log(c, "session thread failed errno=%d", errno);
        ctm_transport_destroy(&c->xport);
        if (c->enet) { enet_client_destroy(c->enet); c->enet = NULL; }
        if (c->log) { fclose(c->log); c->log = NULL; }
        return -1;
    }
    c->session_started = 1;
    return 0;
}

/* Stop bridging: signal stop, wake + join the session thread, then close the
 * HID fd, transport, evdev grabs, and log. When: the user clicks Plug out, or
 * the device disconnects. */
void ctm_controller_plug_out(ctm_controller_t *c)
{
    if (!c) return;
    c->stop = 1;
    if (c->xport.fd >= 0) shutdown(c->xport.fd, SHUT_RDWR);
    if (c->wake_pipe[1] >= 0) (void)write(c->wake_pipe[1], "x", 1);
    if (c->session_started) {
        pthread_join(c->session_thread, NULL);
        c->session_started = 0;
    }
    ctm_transport_disconnect(&c->xport);
    ctm_transport_destroy(&c->xport);
    if (c->hid_fd >= 0) { close(c->hid_fd); c->hid_fd = -1; }
    if (c->dummy_hid_pipe_wr >= 0) { close(c->dummy_hid_pipe_wr); c->dummy_hid_pipe_wr = -1; }
    if (c->wake_pipe[0] >= 0) { close(c->wake_pipe[0]); c->wake_pipe[0] = -1; }
    if (c->wake_pipe[1] >= 0) { close(c->wake_pipe[1]); c->wake_pipe[1] = -1; }
    if (c->enet) { enet_client_destroy(c->enet); c->enet = NULL; }
    release_evdev_grabs(c);
    if (c->log) { fclose(c->log); c->log = NULL; }
}

/* Push new UI settings to the controller (stored + forwarded to ops). When: a
 * detail-window slider/toggle changes while plugged in. */
void ctm_controller_set_settings(ctm_controller_t *c, const tv_bridge_worker_settings_t *s)
{
    if (!c || !s) return;
    pthread_mutex_lock(&c->settings_mutex);
    c->settings = *s;
    pthread_mutex_unlock(&c->settings_mutex);
    if (c->ops->set_settings) c->ops->set_settings(c, s);
}

/* Read the controller's live settings (snapshot). When: UI refresh, or a
 * patch_output hook reading current values. */
void ctm_controller_get_settings(ctm_controller_t *c, tv_bridge_worker_settings_t *out)
{
    if (!c || !out) return;
    *out = copy_settings(c);
}

/* Snapshot live bridging status for the UI panel. When: the UI status timer
 * (~500 ms). connected/transport/last_event are read under status_mutex; the
 * report counters are read advisorily (single-writer, monotonic). */
void ctm_controller_get_status(ctm_controller_t *c, ctm_controller_status_t *out)
{
    if (!c || !out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->status_mutex);
    out->connected = c->st_connected ? true : false;
    out->transport_enet = c->st_transport_enet ? true : false;
    snprintf(out->last_event, sizeof(out->last_event), "%s", c->st_last_event);
    pthread_mutex_unlock(&c->status_mutex);
    out->reports_in = c->st_reports_in;
    out->reports_out = c->st_reports_out;
}

/* Free an idle (already plugged-out) controller. When: the device is removed
 * from the list. */
void ctm_controller_destroy(ctm_controller_t *c)
{
    if (!c) return;
    pthread_mutex_destroy(&c->hid_mutex);
    pthread_mutex_destroy(&c->settings_mutex);
    pthread_mutex_destroy(&c->status_mutex);
    free(c->enum_payload);
    free(c);
}

/* --- factory: specific types first, generic last ------------------------- */

static const ctm_controller_ops_t *const k_registry[] = {
    &ctm_controller_flydigi_ops,
    &ctm_controller_steam_puck_ops,
    &ctm_controller_ds5_ops,
    &ctm_controller_ds4_ops,
    &ctm_controller_xbox_ops,
    &ctm_controller_generic_ops,
};

const ctm_controller_ops_t *ctm_controller_ops_for(const ctm_controller_dev_t *dev)
{
    for (size_t i = 0; i < sizeof(k_registry) / sizeof(k_registry[0]); ++i) {
        const ctm_controller_ops_t *ops = k_registry[i];
        if (ops->matches && ops->matches(dev)) return ops;
    }
    return &ctm_controller_generic_ops;
}
