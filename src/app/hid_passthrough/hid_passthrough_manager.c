#include "hid_passthrough_manager.h"

#if defined(TARGET_WEBOS)

#include "ctm_hid.h"
#include "hid_device.h"
#include "ctm_transport.h"
#include "logging.h"

#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>

#include "enet_transport.h"

static int hid_pt_find_slot_by_phys(hid_passthrough_manager_t *m, const char *phys) {
    if (!phys || !phys[0]) {
        return -1;
    }
    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        if (m->slots[i].in_use && strcmp(m->slots[i].device.phys, phys) == 0) {
            return i;
        }
    }
    return -1;
}

static int hid_pt_find_hidraw_under_input(const char *input_path, char *out, size_t out_len) {
    char hidraw_dir[PATH_MAX];
    snprintf(hidraw_dir, sizeof(hidraw_dir), "%s/device/hidraw", input_path);
    DIR *d = opendir(hidraw_dir);
    if (!d) {
        return -1;
    }
    int found = -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) == 0) {
            snprintf(out, out_len, "/dev/%s", ent->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static int hid_pt_path_in_list(const char paths[][HID_PT_PATH_LEN], int count, const char *path) {
    for (int i = 0; i < count; i++) {
        if (strcmp(paths[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static bool hid_pt_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Walk /sys/class/input like ctm-bridge-webos — more reliable than blind /dev/hidrawN. */
static int hid_pt_collect_hidraw_paths(char paths[][HID_PT_PATH_LEN], int max) {
    int count = 0;
    DIR *dir = opendir("/sys/class/input");
    if (!dir) {
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        if (strncmp(ent->d_name, "input", 5) != 0) {
            continue;
        }
        char input_path[PATH_MAX];
        snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", ent->d_name);
        char path[HID_PT_PATH_LEN];
        if (hid_pt_find_hidraw_under_input(input_path, path, sizeof(path)) != 0) {
            continue;
        }
        if (hid_pt_path_in_list(paths, count, path)) {
            continue;
        }
        strncpy(paths[count], path, HID_PT_PATH_LEN - 1);
        paths[count][HID_PT_PATH_LEN - 1] = '\0';
        count++;
    }
    closedir(dir);

    if (count > 0) {
        return count;
    }

    /* Fallback when sysfs is unavailable. */
    for (int i = 0; i < 32 && count < max; i++) {
        snprintf(paths[count], HID_PT_PATH_LEN, "/dev/hidraw%d", i);
        if (hid_pt_path_exists(paths[count])) {
            count++;
        }
    }
    return count;
}

static uint64_t hid_pt_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

static int hid_pt_find_slot_by_path(hid_passthrough_manager_t *m, const char *path) {
    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        if (m->slots[i].in_use && strcmp(m->slots[i].device.path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int hid_pt_alloc_slot(hid_passthrough_manager_t *m) {
    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        if (!m->slots[i].in_use) {
            return i;
        }
    }
    return -1;
}

static void hid_pt_slot_teardown(hid_pt_slot_t *slot) {
    if (!slot->in_use) {
        return;
    }
    hid_device_close(&slot->device);
    memset(slot, 0, sizeof(*slot));
}

static void hid_pt_disconnect_slot(hid_pt_slot_t *slot) {
    hid_device_t *dev = &slot->device;
    hid_transport_disconnect(&dev->transport);
    dev->connected = false;
    dev->hello_sent = false;
}

static int hid_pt_try_connect_slot(hid_passthrough_manager_t *m, hid_pt_slot_t *slot) {
    hid_device_t *dev = &slot->device;
    if (hid_transport_connected(&dev->transport)) {
        return 0;
    }
    if (hid_transport_connect(&dev->transport, m->host, m->port) != 0) {
        commons_log_warn("HidPassthrough", "Bridge connect failed for %s", dev->path);
        return -1;
    }
    if (!dev->hello_sent && hid_device_send_hello(dev) != 0) {
        commons_log_warn("HidPassthrough", "HELLO failed for %s", dev->path);
        hid_transport_disconnect(&dev->transport);
        return -1;
    }
    dev->connected = true;
    return 0;
}

static void hid_pt_service_slot(hid_passthrough_manager_t *m, hid_pt_slot_t *slot) {
    if (!slot->plugged) {
        return;
    }
    hid_device_t *dev = &slot->device;

    if (!hid_transport_connected(&dev->transport)) {
        if (hid_pt_try_connect_slot(m, slot) != 0) {
            return;
        }
    }

    if (dev->transport_ctx.transport.kind == CTM_TRANSPORT_ENET) {
        uint8_t buf[HID_PT_MAX_REPORT];
        int n = hid_device_read_input(dev, buf, sizeof(buf));
        if (n > 0) {
            hid_transport_send_msg(&dev->transport, CTMB_MSG_INPUT_REPORT, 0, 0, buf, (size_t) n);
        }
        if (hid_transport_service(&dev->transport, 2) < 0) {
            hid_transport_disconnect(&dev->transport);
            dev->connected = false;
            return;
        }
        ctmb_header_t hdr;
        uint8_t *payload = NULL;
        while (hid_transport_recv_msg(&dev->transport, &hdr, &payload) == 1) {
            hid_device_handle_host_message(dev, &hdr, payload);
            free(payload);
            payload = NULL;
        }
        return;
    }

    struct pollfd fds[2];
    fds[0].fd = dev->hidraw_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = dev->transport_ctx.transport.fd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    int pr = poll(fds, 2, 1);
    if (pr < 0) {
        return;
    }
    if (fds[0].revents & POLLIN) {
        uint8_t buf[HID_PT_MAX_REPORT];
        int n = hid_device_read_input(dev, buf, sizeof(buf));
        if (n > 0) {
            if (hid_transport_send_msg(&dev->transport, CTMB_MSG_INPUT_REPORT, 0, 0, buf, (size_t) n) != 0) {
                hid_transport_disconnect(&dev->transport);
                dev->connected = false;
            }
        } else if (n < 0) {
            hid_pt_slot_teardown(slot);
            return;
        }
    }
    if (fds[1].revents & POLLIN) {
        ctmb_header_t hdr;
        uint8_t *payload = NULL;
        int got = hid_transport_recv_msg(&dev->transport, &hdr, &payload);
        if (got <= 0) {
            free(payload);
            hid_transport_disconnect(&dev->transport);
            dev->connected = false;
            return;
        }
        hid_device_handle_host_message(dev, &hdr, payload);
        free(payload);
    }
}

static void hid_pt_scan_devices(hid_passthrough_manager_t *m) {
    char paths[HID_PT_MAX_SLOTS * 2][HID_PT_PATH_LEN];
    int path_count = hid_pt_collect_hidraw_paths(paths, HID_PT_MAX_SLOTS * 2);

    for (int p = 0; p < path_count; p++) {
        const char *path = paths[p];
        if (hid_pt_find_slot_by_path(m, path) >= 0) {
            continue;
        }

        hid_device_t probe;
        if (hid_device_open(&probe, path) != 0) {
            continue;
        }

        int existing = hid_pt_find_slot_by_phys(m, probe.phys);
        if (existing >= 0) {
            hid_pt_slot_t *old = &m->slots[existing];
            int old_score = hid_device_gamepad_score(old->device.vendor_id, old->device.usage_page, old->device.usage);
            int new_score = hid_device_gamepad_score(probe.vendor_id, probe.usage_page, probe.usage);
            if (new_score <= old_score) {
                hid_device_close(&probe);
                continue;
            }
            commons_log_info("HidPassthrough", "Replacing %s with better interface %s", old->device.path, path);
            hid_pt_slot_teardown(old);
        }

        int slot_idx = hid_pt_alloc_slot(m);
        if (slot_idx < 0) {
            hid_device_close(&probe);
            break;
        }
        hid_pt_slot_t *slot = &m->slots[slot_idx];
        slot->device = probe;
        slot->in_use = true;
        commons_log_info("HidPassthrough", "Discovered %s (%04x:%04x %s)", path, slot->device.vendor_id,
                         slot->device.product_id,
                         ctm_hid_usage_label(slot->device.usage_page, slot->device.usage));
        slot->plugged = false;
    }

    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        if (!m->slots[i].in_use) {
            continue;
        }
        if (!hid_pt_path_exists(m->slots[i].device.path)) {
            commons_log_info("HidPassthrough", "Device removed %s", m->slots[i].device.path);
            hid_pt_slot_teardown(&m->slots[i]);
        }
    }
}

static void *hid_pt_worker(void *arg) {
    hid_passthrough_manager_t *m = (hid_passthrough_manager_t *) arg;
    m->last_scan_us = 0;

    while (m->running) {
        uint64_t now = hid_pt_now_us();
        if (now - m->last_scan_us >= (uint64_t) HID_PT_SCAN_INTERVAL_MS * 1000ull) {
            pthread_mutex_lock(&m->mutex);
            hid_pt_scan_devices(m);
            m->last_scan_us = now;
            pthread_mutex_unlock(&m->mutex);
        }

        pthread_mutex_lock(&m->mutex);
        for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
            if (m->slots[i].in_use) {
                hid_pt_service_slot(m, &m->slots[i]);
            }
        }
        pthread_mutex_unlock(&m->mutex);

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&ts, NULL);
    }

    pthread_mutex_lock(&m->mutex);
    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        hid_pt_slot_teardown(&m->slots[i]);
    }
    pthread_mutex_unlock(&m->mutex);
    return NULL;
}

void hid_passthrough_manager_init(hid_passthrough_manager_t *manager) {
    memset(manager, 0, sizeof(*manager));
    pthread_mutex_init(&manager->mutex, NULL);
    manager->port = HID_PT_DEFAULT_PORT;
    manager->enet_global = enet_client_global_init();
}

void hid_passthrough_manager_deinit(hid_passthrough_manager_t *manager) {
    hid_passthrough_manager_stop(manager);
    pthread_mutex_destroy(&manager->mutex);
    if (manager->enet_global == 0) {
        enet_client_global_deinit();
    }
}

int hid_passthrough_manager_start(hid_passthrough_manager_t *manager, const char *host, int port) {
    if (!manager || !host || !host[0]) {
        return -1;
    }
    if (manager->running) {
        return 0;
    }
    strncpy(manager->host, host, sizeof(manager->host) - 1);
    manager->port = port > 0 ? port : HID_PT_DEFAULT_PORT;
    manager->running = true;
    if (pthread_create(&manager->thread, NULL, hid_pt_worker, manager) != 0) {
        manager->running = false;
        return -1;
    }
    manager->thread_started = true;
    commons_log_info("HidPassthrough", "Started bridge to %s:%d", manager->host, manager->port);
    return 0;
}

void hid_passthrough_manager_stop(hid_passthrough_manager_t *manager) {
    if (!manager || !manager->running) {
        return;
    }
    manager->running = false;
    if (manager->thread_started) {
        pthread_join(manager->thread, NULL);
        manager->thread_started = false;
    }
    commons_log_info("HidPassthrough", "Stopped");
}

bool hid_passthrough_manager_active(const hid_passthrough_manager_t *manager) {
    return manager && manager->running;
}

int hid_passthrough_manager_device_count(hid_passthrough_manager_t *manager) {
    if (!manager) {
        return 0;
    }
    int count = 0;
    pthread_mutex_lock(&manager->mutex);
    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        if (manager->slots[i].in_use) {
            count++;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return count;
}

int hid_passthrough_manager_get_device(hid_passthrough_manager_t *manager, int index,
                                       hid_pt_device_info_t *info) {
    if (!manager || !info || index < 0) {
        return -1;
    }
    int found = -1;
    pthread_mutex_lock(&manager->mutex);
    for (int i = 0; i < HID_PT_MAX_SLOTS; i++) {
        if (!manager->slots[i].in_use) {
            continue;
        }
        found++;
        if (found == index) {
            const hid_device_t *dev = &manager->slots[i].device;
            memset(info, 0, sizeof(*info));
            strncpy(info->path, dev->path, sizeof(info->path) - 1);
            strncpy(info->product, dev->caps.product, sizeof(info->product) - 1);
            info->vendor_id = dev->vendor_id;
            info->product_id = dev->product_id;
            info->usage_page = dev->usage_page;
            info->usage = dev->usage;
            info->plugged = manager->slots[i].plugged;
            info->connected = dev->connected && hid_transport_connected(&dev->transport);
            pthread_mutex_unlock(&manager->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return -1;
}

int hid_passthrough_manager_plug(hid_passthrough_manager_t *manager, const char *path) {
    if (!manager || !path) {
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&manager->mutex);
    int idx = hid_pt_find_slot_by_path(manager, path);
    if (idx >= 0) {
        hid_pt_slot_t *slot = &manager->slots[idx];
        slot->plugged = true;
        rc = hid_pt_try_connect_slot(manager, slot);
        if (rc != 0) {
            slot->plugged = false;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return rc;
}

int hid_passthrough_manager_unplug(hid_passthrough_manager_t *manager, const char *path) {
    if (!manager || !path) {
        return -1;
    }
    pthread_mutex_lock(&manager->mutex);
    int idx = hid_pt_find_slot_by_path(manager, path);
    if (idx >= 0) {
        manager->slots[idx].plugged = false;
        hid_pt_disconnect_slot(&manager->slots[idx]);
    }
    pthread_mutex_unlock(&manager->mutex);
    return idx >= 0 ? 0 : -1;
}

void hid_passthrough_manager_rescan(hid_passthrough_manager_t *manager) {
    if (!manager || !manager->running) {
        return;
    }
    pthread_mutex_lock(&manager->mutex);
    hid_pt_scan_devices(manager);
    pthread_mutex_unlock(&manager->mutex);
}

#endif
