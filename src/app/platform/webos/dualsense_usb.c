#include "dualsense_usb.h"

#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <SDL.h>
#include <Limelight.h>

/* Sony DualSense / DualSense Edge */
#define DS_VENDOR 0x054cu
#define DS_PID_DS5 0x0ce6u
#define DS_PID_EDGE 0x0df2u
#define BUS_USB 0x03u

/* Hardcode ioctl numbers — webOS NDK may lack linux/hidraw.h. */
#define HIDIOCGRAWINFO ((2U << 30) | (8U << 16) | ((unsigned) ('H') << 8) | 0x03U)
#define HIDIOCGRAWUNIQ(len) ((2U << 30) | ((unsigned) (len) << 16) | ((unsigned) ('H') << 8) | 0x08U)

#define DS_USB_REPORT_LEN 63
#define DS_COMMON_OFF 1
#define DS_COMMON_LEN 47
#define DS_EFFECT_LEN 11

#define FLAG0_RIGHT_TRIGGER 0x04u
#define FLAG0_LEFT_TRIGGER 0x08u
#define FLAG1_MIC_LED 0x01u
#define FLAG1_LIGHTBAR 0x04u
#define FLAG1_PLAYER_LEDS 0x10u

#define OFF_MIC_LED 8
#define OFF_RIGHT_TRIGGER 10
#define OFF_LEFT_TRIGGER 21
#define OFF_PLAYER_LEDS 43
#define OFF_LIGHTBAR_RED 44

#define MAX_HIDRAW_NODES 10

struct hidraw_devinfo {
    uint32_t bustype;
    int16_t vendor;
    int16_t product;
};

struct dualsense_usb_t {
    int fd;
    char path[32];
    bool triggers_owned;
    bool lightbar_owned;
    bool player_led_owned;
    bool mic_led_owned;
    uint8_t left_effect[DS_EFFECT_LEN];
    uint8_t right_effect[DS_EFFECT_LEN];
    uint8_t lightbar[3];
    uint8_t player_leds;
    uint8_t mic_led;
    bool write_fail_logged;
};

static void normalize_mac(char *dst, size_t dst_len, const char *src) {
    size_t o = 0;
    if (src == NULL || dst_len == 0) {
        if (dst_len > 0) {
            dst[0] = '\0';
        }
        return;
    }
    for (const char *p = src; *p && o + 1 < dst_len; p++) {
        char c = *p;
        if (c == '-' || c == ':') {
            continue;
        }
        if (c >= 'A' && c <= 'F') {
            c = (char) (c - 'A' + 'a');
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

static bool hidraw_read_uniq(int fd, char *out, size_t out_len) {
    char buf[128];
    memset(buf, 0, sizeof(buf));
    if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(buf)), buf) < 0) {
        return false;
    }
    normalize_mac(out, out_len, buf);
    return out[0] != '\0';
}

static bool hidraw_is_wired_dualsense(int fd) {
    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        return false;
    }
    if (info.bustype != BUS_USB) {
        return false;
    }
    uint16_t vendor = (uint16_t) info.vendor;
    uint16_t product = (uint16_t) info.product;
    return vendor == DS_VENDOR && (product == DS_PID_DS5 || product == DS_PID_EDGE);
}

static int open_wired_node(const char *path) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (!hidraw_is_wired_dualsense(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool serials_match(const char *a, const char *b) {
    char na[32], nb[32];
    normalize_mac(na, sizeof(na), a);
    normalize_mac(nb, sizeof(nb), b);
    return na[0] != '\0' && strcmp(na, nb) == 0;
}

static void fill_common(uint8_t *c, const dualsense_usb_t *ds) {
    memset(c, 0, DS_COMMON_LEN);
    if (ds->triggers_owned) {
        c[0] = (uint8_t) (FLAG0_RIGHT_TRIGGER | FLAG0_LEFT_TRIGGER);
        memcpy(c + OFF_RIGHT_TRIGGER, ds->right_effect, DS_EFFECT_LEN);
        memcpy(c + OFF_LEFT_TRIGGER, ds->left_effect, DS_EFFECT_LEN);
    }
    if (ds->mic_led_owned) {
        c[1] |= FLAG1_MIC_LED;
        c[OFF_MIC_LED] = ds->mic_led;
    }
    if (ds->lightbar_owned) {
        c[1] |= FLAG1_LIGHTBAR;
        c[OFF_LIGHTBAR_RED] = ds->lightbar[0];
        c[OFF_LIGHTBAR_RED + 1] = ds->lightbar[1];
        c[OFF_LIGHTBAR_RED + 2] = ds->lightbar[2];
    }
    if (ds->player_led_owned) {
        c[1] |= FLAG1_PLAYER_LEDS;
        c[OFF_PLAYER_LEDS] = (uint8_t) (ds->player_leds & 0x1Fu);
    }
}

static bool flush_report(dualsense_usb_t *ds) {
    uint8_t report[DS_USB_REPORT_LEN];
    memset(report, 0, sizeof(report));
    report[0] = 0x02;
    fill_common(report + DS_COMMON_OFF, ds);

    ssize_t n = write(ds->fd, report, sizeof(report));
    if (n != (ssize_t) sizeof(report)) {
        if (!ds->write_fail_logged) {
            commons_log_warn("Input", "DualSense USB write to %s failed: %s (further errors quiet)",
                             ds->path, n < 0 ? strerror(errno) : "short write");
            ds->write_fail_logged = true;
        }
        return false;
    }
    ds->write_fail_logged = false;
    return true;
}

dualsense_usb_t *dualsense_usb_open(SDL_GameController *controller) {
    if (controller == NULL) {
        return NULL;
    }
#if SDL_VERSION_ATLEAST(2, 0, 16)
    if (SDL_GameControllerGetType(controller) != SDL_CONTROLLER_TYPE_PS5) {
        return NULL;
    }
#else
    return NULL;
#endif

    SDL_Joystick *js = SDL_GameControllerGetJoystick(controller);
    const char *serial = NULL;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    serial = js != NULL ? SDL_JoystickGetSerial(js) : NULL;
#endif

    int fd = -1;
    char path[32] = {0};

#if SDL_VERSION_ATLEAST(2, 0, 18)
    const char *sdl_path = SDL_GameControllerPath(controller);
    if (sdl_path != NULL && strncmp(sdl_path, "/dev/hidraw", 11) == 0) {
        fd = open_wired_node(sdl_path);
        if (fd >= 0) {
            snprintf(path, sizeof(path), "%s", sdl_path);
        }
    }
#endif

    if (fd < 0) {
        int fallback_fd = -1;
        char fallback_path[32] = {0};
        for (int i = 0; i < MAX_HIDRAW_NODES; i++) {
            char candidate[32];
            snprintf(candidate, sizeof(candidate), "/dev/hidraw%d", i);
            int try_fd = open_wired_node(candidate);
            if (try_fd < 0) {
                continue;
            }
            if (serial != NULL && serial[0] != '\0') {
                char uniq[32];
                if (hidraw_read_uniq(try_fd, uniq, sizeof(uniq)) && serials_match(uniq, serial)) {
                    fd = try_fd;
                    snprintf(path, sizeof(path), "%s", candidate);
                    if (fallback_fd >= 0) {
                        close(fallback_fd);
                        fallback_fd = -1;
                    }
                    break;
                }
                if (fallback_fd < 0) {
                    fallback_fd = try_fd;
                    snprintf(fallback_path, sizeof(fallback_path), "%s", candidate);
                } else {
                    close(try_fd);
                }
                continue;
            }
            fd = try_fd;
            snprintf(path, sizeof(path), "%s", candidate);
            break;
        }
        if (fd < 0 && fallback_fd >= 0) {
            fd = fallback_fd;
            snprintf(path, sizeof(path), "%s", fallback_path);
        } else if (fallback_fd >= 0) {
            close(fallback_fd);
        }
    }

    if (fd < 0) {
        commons_log_info("Input", "DualSense USB hidraw unavailable (Bluetooth or no node)");
        return NULL;
    }

    dualsense_usb_t *ds = calloc(1, sizeof(*ds));
    if (ds == NULL) {
        close(fd);
        return NULL;
    }
    ds->fd = fd;
    snprintf(ds->path, sizeof(ds->path), "%s", path);
    commons_log_info("Input", "DualSense USB feedback on %s", ds->path);
    return ds;
}

void dualsense_usb_close(dualsense_usb_t *ds) {
    if (ds == NULL) {
        return;
    }
    /* Release trigger resistance so the pad is not left stiff after disconnect. */
    ds->triggers_owned = true;
    memset(ds->left_effect, 0, sizeof(ds->left_effect));
    memset(ds->right_effect, 0, sizeof(ds->right_effect));
    (void) flush_report(ds);
    close(ds->fd);
    free(ds);
}

bool dualsense_usb_set_adaptive_triggers(dualsense_usb_t *ds, uint8_t eventFlags,
                                         uint8_t typeLeft, uint8_t typeRight,
                                         const uint8_t *left, const uint8_t *right) {
    if (ds == NULL) {
        return false;
    }
    if (eventFlags & DS_EFFECT_RIGHT_TRIGGER) {
        ds->right_effect[0] = typeRight;
        if (right != NULL) {
            memcpy(ds->right_effect + 1, right, DS_EFFECT_PAYLOAD_SIZE);
        }
    }
    if (eventFlags & DS_EFFECT_LEFT_TRIGGER) {
        ds->left_effect[0] = typeLeft;
        if (left != NULL) {
            memcpy(ds->left_effect + 1, left, DS_EFFECT_PAYLOAD_SIZE);
        }
    }
    ds->triggers_owned = true;
    return flush_report(ds);
}

bool dualsense_usb_set_lightbar(dualsense_usb_t *ds, uint8_t r, uint8_t g, uint8_t b) {
    if (ds == NULL) {
        return false;
    }
    ds->lightbar[0] = r;
    ds->lightbar[1] = g;
    ds->lightbar[2] = b;
    ds->lightbar_owned = true;
    return flush_report(ds);
}

bool dualsense_usb_set_player_led(dualsense_usb_t *ds, uint8_t ledValue) {
    if (ds == NULL) {
        return false;
    }
    ds->player_leds = ledValue;
    ds->player_led_owned = true;
    return flush_report(ds);
}

bool dualsense_usb_set_mic_led(dualsense_usb_t *ds, uint8_t ledState) {
    if (ds == NULL) {
        return false;
    }
    ds->mic_led = ledState;
    ds->mic_led_owned = true;
    return flush_report(ds);
}
