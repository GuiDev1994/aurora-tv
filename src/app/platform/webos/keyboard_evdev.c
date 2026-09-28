#include "keyboard_evdev.h"

#include "logging.h"
#include "stream/input/vk.h"

#include <Limelight.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <SDL.h>

#define KBD_EVDEV_MAX_FDS 8
#define KBD_EVDEV_BUSY_MS 250u

struct keyboard_evdev_t {
    int fds[KBD_EVDEV_MAX_FDS];
    int nfds;
    pthread_t thread;
    bool thread_started;
    volatile int running;
    keyboard_evdev_key_fn cb;
    void *userdata;
    Uint32 last_event_ticks;
    bool shift;
    bool ctrl;
    bool alt;
    bool meta;
};

static inline bool has_bit(const unsigned char *bits, unsigned bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

static bool name_denied(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return true;
    }
    /* Magic Remote / LG virtual keys must stay on SDL. */
    if (strstr(name, "LGE") != NULL || strstr(name, "M-RCU") != NULL ||
        strstr(name, "Builtin") != NULL || strstr(name, "gpio") != NULL) {
        return true;
    }
    return false;
}

static bool is_usb_keyboard(int fd) {
    char name[256];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        name[0] = '\0';
    }
    if (name_denied(name)) {
        return false;
    }

    unsigned char keycaps[(KEY_MAX / 8) + 1];
    memset(keycaps, 0, sizeof(keycaps));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keycaps)), keycaps) < 0) {
        return false;
    }
    /* punktfunk filter: real keyboard advertises letters + left ctrl. */
    if (!has_bit(keycaps, KEY_A) || !has_bit(keycaps, KEY_LEFTCTRL)) {
        return false;
    }
    /* Skip pure mice that also report a few keys. */
    unsigned char relcaps[(REL_MAX / 8) + 1];
    memset(relcaps, 0, sizeof(relcaps));
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relcaps)), relcaps) == 0 &&
        has_bit(relcaps, REL_X) && has_bit(relcaps, REL_Y) && has_bit(keycaps, BTN_LEFT) &&
        !has_bit(keycaps, KEY_Q)) {
        return false;
    }
    commons_log_info("Input", "Keyboard evdev: claiming '%s'", name[0] ? name : "?");
    return true;
}

static short linux_key_to_vk(unsigned code) {
    switch (code) {
        case KEY_ESC: return VK_ESCAPE;
        case KEY_1: return VK_1;
        case KEY_2: return VK_2;
        case KEY_3: return VK_3;
        case KEY_4: return VK_4;
        case KEY_5: return VK_5;
        case KEY_6: return VK_6;
        case KEY_7: return VK_7;
        case KEY_8: return VK_8;
        case KEY_9: return VK_9;
        case KEY_0: return VK_0;
        case KEY_MINUS: return VK_OEM_MINUS;
        case KEY_EQUAL: return 0xBB; /* VK_OEM_PLUS */
        case KEY_BACKSPACE: return VK_BACK;
        case KEY_TAB: return VK_TAB;
        case KEY_Q: return VK_Q;
        case KEY_W: return VK_W;
        case KEY_E: return VK_E;
        case KEY_R: return VK_R;
        case KEY_T: return VK_T;
        case KEY_Y: return VK_Y;
        case KEY_U: return VK_U;
        case KEY_I: return VK_I;
        case KEY_O: return VK_O;
        case KEY_P: return VK_P;
        case KEY_LEFTBRACE: return VK_OEM_4;
        case KEY_RIGHTBRACE: return VK_OEM_6;
        case KEY_ENTER:
        case KEY_KPENTER: return VK_RETURN;
        case KEY_LEFTCTRL: return VK_LCONTROL;
        case KEY_RIGHTCTRL: return VK_RCONTROL;
        case KEY_A: return VK_A;
        case KEY_S: return VK_S;
        case KEY_D: return VK_D;
        case KEY_F: return VK_F;
        case KEY_G: return VK_G;
        case KEY_H: return VK_H;
        case KEY_J: return VK_J;
        case KEY_K: return VK_K;
        case KEY_L: return VK_L;
        case KEY_SEMICOLON: return VK_OEM_1;
        case KEY_APOSTROPHE: return VK_OEM_7;
        case KEY_GRAVE: return VK_OEM_3;
        case KEY_LEFTSHIFT: return VK_LSHIFT;
        case KEY_RIGHTSHIFT: return VK_RSHIFT;
        case KEY_BACKSLASH: return VK_OEM_5;
        case KEY_Z: return VK_Z;
        case KEY_X: return VK_X;
        case KEY_C: return VK_C;
        case KEY_V: return VK_V;
        case KEY_B: return VK_B;
        case KEY_N: return VK_N;
        case KEY_M: return VK_M;
        case KEY_COMMA: return VK_OEM_COMMA;
        case KEY_DOT: return VK_OEM_PERIOD;
        case KEY_SLASH: return VK_OEM_2;
        case KEY_LEFTALT: return VK_LMENU;
        case KEY_RIGHTALT: return VK_RMENU;
        case KEY_SPACE: return VK_SPACE;
        case KEY_CAPSLOCK: return VK_CAPITAL;
        case KEY_F1: return VK_F1;
        case KEY_F2: return VK_F2;
        case KEY_F3: return VK_F3;
        case KEY_F4: return VK_F4;
        case KEY_F5: return VK_F5;
        case KEY_F6: return VK_F6;
        case KEY_F7: return VK_F7;
        case KEY_F8: return VK_F8;
        case KEY_F9: return VK_F9;
        case KEY_F10: return VK_F10;
        case KEY_F11: return VK_F11;
        case KEY_F12: return VK_F12;
        case KEY_NUMLOCK: return VK_NUMLOCK;
        case KEY_SCROLLLOCK: return VK_SCROLL;
        case KEY_KP0: return VK_NUMPAD0;
        case KEY_KP1: return VK_NUMPAD1;
        case KEY_KP2: return VK_NUMPAD2;
        case KEY_KP3: return VK_NUMPAD3;
        case KEY_KP4: return VK_NUMPAD4;
        case KEY_KP5: return VK_NUMPAD5;
        case KEY_KP6: return VK_NUMPAD6;
        case KEY_KP7: return VK_NUMPAD7;
        case KEY_KP8: return VK_NUMPAD8;
        case KEY_KP9: return VK_NUMPAD9;
        case KEY_KPASTERISK: return VK_MULTIPLY;
        case KEY_KPMINUS: return VK_SUBTRACT;
        case KEY_KPPLUS: return VK_ADD;
        case KEY_KPDOT: return VK_DECIMAL;
        case KEY_KPSLASH: return VK_DIVIDE;
        case KEY_102ND: return VK_OEM_102;
        case KEY_SYSRQ: return VK_SNAPSHOT;
        case KEY_PAUSE: return VK_PAUSE;
        case KEY_HOME: return VK_HOME;
        case KEY_UP: return VK_UP;
        case KEY_PAGEUP: return VK_PRIOR;
        case KEY_LEFT: return VK_LEFT;
        case KEY_RIGHT: return VK_RIGHT;
        case KEY_END: return VK_END;
        case KEY_DOWN: return VK_DOWN;
        case KEY_PAGEDOWN: return VK_NEXT;
        case KEY_INSERT: return VK_INSERT;
        case KEY_DELETE: return VK_DELETE;
        case KEY_LEFTMETA: return VK_LWIN;
        case KEY_RIGHTMETA: return VK_RWIN;
#ifdef KEY_COMPOSE
        case KEY_COMPOSE: return VK_APPS;
#endif
#ifdef KEY_MENU
        case KEY_MENU: return VK_APPS;
#endif
        default: return 0;
    }
}

static char current_modifiers(const keyboard_evdev_t *kbd) {
    char m = 0;
    if (kbd->shift) {
        m |= MODIFIER_SHIFT;
    }
    if (kbd->ctrl) {
        m |= MODIFIER_CTRL;
    }
    if (kbd->alt) {
        m |= MODIFIER_ALT;
    }
    if (kbd->meta) {
        m |= MODIFIER_META;
    }
    return m;
}

static void update_modifier(keyboard_evdev_t *kbd, unsigned code, bool down) {
    switch (code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            kbd->shift = down;
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            kbd->ctrl = down;
            break;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            kbd->alt = down;
            break;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            kbd->meta = down;
            break;
        default:
            break;
    }
}

static void handle_key_event(keyboard_evdev_t *kbd, const struct input_event *ev) {
    if (ev->type != EV_KEY || ev->value == 2) {
        /* value 2 = kernel autorepeat; Limelight path ignores repeats. */
        return;
    }
    bool down = ev->value != 0;
    short vk = linux_key_to_vk(ev->code);
    if (vk == 0) {
        return;
    }
    update_modifier(kbd, ev->code, down);
    kbd->last_event_ticks = SDL_GetTicks();
    if (kbd->cb != NULL) {
        if (vk == VK_HOME || vk == VK_INSERT || vk == VK_F6 ||
            (vk >= VK_F1 && vk <= VK_F12)) {
            commons_log_info("Input", "Keyboard evdev %s vk=0x%02x linux=%u",
                             down ? "DOWN" : "UP", (unsigned) vk, ev->code);
        }
        kbd->cb(vk, down, current_modifiers(kbd), kbd->userdata);
    }
}

static void *keyboard_evdev_thread(void *arg) {
    keyboard_evdev_t *kbd = arg;
    while (kbd->running) {
        fd_set set;
        FD_ZERO(&set);
        int maxfd = -1;
        for (int i = 0; i < kbd->nfds; i++) {
            FD_SET(kbd->fds[i], &set);
            if (kbd->fds[i] > maxfd) {
                maxfd = kbd->fds[i];
            }
        }
        if (maxfd < 0) {
            break;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        int rc = select(maxfd + 1, &set, NULL, NULL, &tv);
        if (rc <= 0) {
            continue;
        }
        for (int i = 0; i < kbd->nfds; i++) {
            if (!FD_ISSET(kbd->fds[i], &set)) {
                continue;
            }
            struct input_event ev;
            ssize_t n = read(kbd->fds[i], &ev, sizeof(ev));
            if (n != (ssize_t) sizeof(ev)) {
                continue;
            }
            handle_key_event(kbd, &ev);
        }
    }
    return NULL;
}

static int open_keyboards(keyboard_evdev_t *kbd) {
    DIR *dir = opendir("/dev/input");
    if (dir == NULL) {
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while (n < KBD_EVDEV_MAX_FDS && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (!is_usb_keyboard(fd)) {
            close(fd);
            continue;
        }
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            commons_log_warn("Input", "Keyboard evdev: EVIOCGRAB failed on %s: %s",
                             path, strerror(errno));
            close(fd);
            continue;
        }
        kbd->fds[n++] = fd;
        commons_log_info("Input", "Keyboard evdev: grabbed %s", path);
    }
    closedir(dir);
    return n;
}

keyboard_evdev_t *keyboard_evdev_start(keyboard_evdev_key_fn cb, void *userdata) {
    keyboard_evdev_t *kbd = calloc(1, sizeof(*kbd));
    if (kbd == NULL) {
        return NULL;
    }
    for (int i = 0; i < KBD_EVDEV_MAX_FDS; i++) {
        kbd->fds[i] = -1;
    }
    kbd->cb = cb;
    kbd->userdata = userdata;
    kbd->nfds = open_keyboards(kbd);
    if (kbd->nfds <= 0) {
        commons_log_info("Input", "Keyboard evdev: no USB keyboard node to grab");
        free(kbd);
        return NULL;
    }
    kbd->running = 1;
    kbd->thread_started = false;
    if (pthread_create(&kbd->thread, NULL, keyboard_evdev_thread, kbd) != 0) {
        commons_log_error("Input", "Keyboard evdev: thread create failed");
        keyboard_evdev_stop(kbd);
        return NULL;
    }
    kbd->thread_started = true;
    return kbd;
}

void keyboard_evdev_stop(keyboard_evdev_t *kbd) {
    if (kbd == NULL) {
        return;
    }
    kbd->running = 0;
    if (kbd->thread_started) {
        pthread_join(kbd->thread, NULL);
        kbd->thread_started = false;
    }
    for (int i = 0; i < kbd->nfds; i++) {
        if (kbd->fds[i] >= 0) {
            ioctl(kbd->fds[i], EVIOCGRAB, 0);
            close(kbd->fds[i]);
            kbd->fds[i] = -1;
        }
    }
    free(kbd);
}

bool keyboard_evdev_busy(const keyboard_evdev_t *kbd) {
    if (kbd == NULL || kbd->last_event_ticks == 0) {
        return false;
    }
    return (SDL_GetTicks() - kbd->last_event_ticks) < KBD_EVDEV_BUSY_MS;
}
