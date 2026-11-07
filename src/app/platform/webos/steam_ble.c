#include "steam_ble.h"

#include "logging.h"
#include "lunasynccall.h"

#include <pbnjson.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define URI_BT_STATUS "luna://com.webos.service.bluetooth2/device/getStatus"
#define URI_GATT_CONNECT "luna://com.webos.service.blegatt/client/connect"
#define URI_GATT_REQUEST_MTU "luna://com.webos.service.blegatt/client/requestMtu"
#define URI_GATT_DISCONNECT "luna://com.webos.service.blegatt/client/disconnect"

/*
 * The blegatt calls go through luna-send-pub. client/requestMtu is in the "public" and
 * "bluetooth.manage" groups only, and an app installed in developer mode gets neither
 * (declaring requiredPermissions does not change that): the hub answers "Denied method
 * call". luna-send-pub has the "public" group and is in every app's jail. blegatt keeps one
 * GATT client per address and caller, so connect, requestMtu and disconnect all have to
 * come from it.
 */
#define LUNA_SEND_PUB "/usr/bin/luna-send-pub"

/* The controller answers 247 with 67, and input starts right away. */
#define REQUEST_MTU 247
#define SCAN_MS 3000
/* A controller that refuses the exchange is not asked again on every scan. */
#define MAX_ATTEMPTS 3
#define MAX_PADS 4
#define ADDR_LEN 18
/* blegatt's errorCode when a client for the address already exists. */
#define ERROR_ALREADY_EXISTS 4
/* Valve's USB vendor id, which its HID over GATT devices report too. */
#define VALVE_VENDOR "Vendor=28de"

typedef struct {
    bool used;
    char addr[ADDR_LEN];
    /*
     * luna-send-pub holding the client/connect subscription, and the read end of its output.
     * The subscription has to stay open: ending it ends the GATT client. While it runs,
     * client/disconnect is owed when the controller goes or the app exits.
     */
    pid_t conn_pid;
    int conn_fd;
    char line[1024];
    size_t line_len;
    bool connected;
    bool mtu_requested;
    bool mtu_done;
    int attempts;
} steam_pad_t;

struct webos_steam_ble_state {
    atomic_bool stop;
    atomic_bool finished;
    pthread_t thread;
    steam_pad_t pads[MAX_PADS];
};

typedef struct {
    char addr[MAX_PADS][ADDR_LEN];
    int count;
} addr_list_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static void addr_normalize(char *dst, const char *src, size_t n) {
    if (n >= ADDR_LEN) {
        n = ADDR_LEN - 1;
    }
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        dst[i] = (char) (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    dst[n] = '\0';
}

static bool list_contains(const addr_list_t *list, const char *addr) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->addr[i], addr) == 0) {
            return true;
        }
    }
    return false;
}

static void list_add(addr_list_t *list, const char *addr, size_t len) {
    char normalized[ADDR_LEN];
    addr_normalize(normalized, addr, len);
    if (normalized[0] == '\0' || list->count >= MAX_PADS || list_contains(list, normalized)) {
        return;
    }
    memcpy(list->addr[list->count++], normalized, ADDR_LEN);
}

static jvalue_ref parse_json(const char *json, jdomparser_ref *parser) {
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    *parser = jdomparser_create(&schema, 0);
    if (*parser == NULL) {
        return NULL;
    }
    if (!jdomparser_feed(*parser, json, (int) strlen(json)) || !jdomparser_end(*parser)) {
        jdomparser_release(parser);
        return NULL;
    }
    return jdomparser_get_result(*parser);
}

static bool raw_contains_steam(raw_buffer buf) {
    static const char needle[] = "steam";
    const size_t needle_len = sizeof(needle) - 1;
    for (long i = 0; buf.m_str != NULL && i + (long) needle_len <= buf.m_len; i++) {
        if (strncasecmp(buf.m_str + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/* Paired devices the Bluetooth service reports as connected and named like a Steam Controller. */
static void scan_bt_status(addr_list_t *out) {
    char *reply = NULL;
    if (!HLunaServiceCallSync(URI_BT_STATUS, "{}", true, &reply) || reply == NULL) {
        free(reply);
        return;
    }
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_json(reply, &parser);
    free(reply);
    if (root == NULL) {
        return;
    }
    jvalue_ref devices = jobject_get(root, J_CSTR_TO_BUF("devices"));
    for (ssize_t i = 0; jis_array(devices) && i < jarray_size(devices); i++) {
        jvalue_ref device = jarray_get(devices, i);
        jvalue_ref name = jobject_get(device, J_CSTR_TO_BUF("name"));
        jvalue_ref address = jobject_get(device, J_CSTR_TO_BUF("address"));
        jvalue_ref profiles = jobject_get(device, J_CSTR_TO_BUF("connectedProfiles"));
        if (!jis_string(name) || !jis_string(address) || !jis_array(profiles) || jarray_size(profiles) == 0) {
            continue;
        }
        if (raw_contains_steam(jstring_get_fast(name))) {
            raw_buffer addr = jstring_get_fast(address);
            list_add(out, addr.m_str, (size_t) addr.m_len);
        }
    }
    jdomparser_release(&parser);
}

/* Bluetooth (Bus=0005) input nodes from Valve, in case the service names the controller differently. */
static void scan_input_devices(addr_list_t *out) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    bool valve_bt = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '\n') {
            valve_bt = false;
        } else if (strncmp(line, "I: Bus=0005", 11) == 0) {
            valve_bt = strstr(line, VALVE_VENDOR) != NULL;
        } else if (valve_bt && strncmp(line, "U: Uniq=", 8) == 0) {
            list_add(out, line + 8, strcspn(line + 8, " \r\n"));
        }
    }
    fclose(f);
}

/*
 * Starts a luna-send-pub subscription ("-i") with its output on a pipe; returns its pid, or
 * -1. It gets SIGTERM if the thread that started it dies, so a crash does not leave a GATT
 * client behind.
 */
static pid_t luna_send_pub_subscribe(const char *uri, const char *payload, int *out_fd) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(fds[1], STDOUT_FILENO);
        execl(LUNA_SEND_PUB, "luna-send-pub", "-i", uri, payload, (char *) NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return -1;
    }
    *out_fd = fds[0];
    return pid;
}

static bool json_bool(jvalue_ref obj, const char *key) {
    bool value = false;
    jvalue_ref v = jobject_get(obj, j_cstr_to_buffer(key));
    if (jis_boolean(v)) {
        jboolean_get(v, &value);
    }
    return value;
}

/* Reads one reply object: returnValue, and errorText (may be empty) on failure. */
static bool reply_ok(const char *json, char *error, size_t error_len) {
    if (error_len > 0) {
        error[0] = '\0';
    }
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_json(json, &parser);
    if (root == NULL) {
        snprintf(error, error_len, "%s", json);
        return false;
    }
    bool ok = json_bool(root, "returnValue");
    jvalue_ref text = jobject_get(root, J_CSTR_TO_BUF("errorText"));
    if (!ok && jis_string(text)) {
        raw_buffer buf = jstring_get_fast(text);
        snprintf(error, error_len, "%.*s", (int) buf.m_len, buf.m_str);
    }
    jdomparser_release(&parser);
    return ok;
}

/* Calls a one-shot blegatt method. On failure, errorText goes to error (may be empty). */
static bool gatt_call(const char *uri, const char *payload, char *error, size_t error_len) {
    char command[256], reply[1024];
    snprintf(command, sizeof(command), LUNA_SEND_PUB " -n 1 %s '%s'", uri, payload);
    FILE *f = popen(command, "r");
    if (f == NULL || fgets(reply, sizeof(reply), f) == NULL) {
        if (f != NULL) {
            pclose(f);
        }
        snprintf(error, error_len, "no reply");
        return false;
    }
    pclose(f);
    return reply_ok(reply, error, error_len);
}

static void open_client(steam_pad_t *pad) {
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"address\":\"%s\",\"subscribe\":true}", pad->addr);
    pad->connected = false;
    pad->mtu_requested = false;
    pad->line_len = 0;
    pad->conn_pid = luna_send_pub_subscribe(URI_GATT_CONNECT, payload, &pad->conn_fd);
    if (pad->conn_pid < 0) {
        commons_log_warn("SteamBLE", "client/connect %s: luna-send-pub did not start", pad->addr);
        return;
    }
    fcntl(pad->conn_fd, F_SETFL, fcntl(pad->conn_fd, F_GETFL) | O_NONBLOCK);
}

/* Ends the GATT client: client/disconnect if it was connected, then stops luna-send-pub. */
static void close_client(steam_pad_t *pad, bool disconnect) {
    if (pad->conn_pid <= 0) {
        return;
    }
    if (disconnect) {
        char payload[64], error[128];
        snprintf(payload, sizeof(payload), "{\"address\":\"%s\"}", pad->addr);
        if (!gatt_call(URI_GATT_DISCONNECT, payload, error, sizeof(error))) {
            commons_log_warn("SteamBLE", "client/disconnect %s failed: %s", pad->addr, error);
        }
    }
    kill(pad->conn_pid, SIGTERM);
    waitpid(pad->conn_pid, NULL, 0);
    close(pad->conn_fd);
    pad->conn_pid = -1;
    pad->conn_fd = -1;
}

/*
 * One message on the connect subscription. The first is only
 * {"event":"onConnectionStateChange","values":{"connecting":true,"connected":false}}; the
 * link is up at "connected":true, and the exchange is done at "onRequestMtu". Errors come
 * as returnValue false. Returns false when the subscription failed.
 */
static bool on_connect_message(steam_pad_t *pad, const char *json) {
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_json(json, &parser);
    if (root == NULL) {
        return true;
    }
    bool ok = json_bool(root, "returnValue");
    jvalue_ref event = jobject_get(root, J_CSTR_TO_BUF("event"));
    jvalue_ref values = jobject_get(root, J_CSTR_TO_BUF("values"));
    if (!ok) {
        int32_t code = 0;
        jvalue_ref code_value = jobject_get(root, J_CSTR_TO_BUF("errorCode"));
        if (jis_number(code_value)) {
            jnumber_get_i32(code_value, &code);
        }
        if (code == ERROR_ALREADY_EXISTS) {
            /* Another app has a GATT client for it (and has probably done the exchange), or one
             * was left behind; client/disconnect does not end it while the controller stays
             * connected. Leave it alone; it clears when the controller reconnects. */
            commons_log_info("SteamBLE", "client/connect %s: another client exists, leaving it", pad->addr);
            pad->attempts = MAX_ATTEMPTS;
        } else {
            commons_log_warn("SteamBLE", "client/connect %s failed (attempt %d): %s", pad->addr, pad->attempts,
                             json);
        }
    } else if (jis_string(event) && jstring_equal2(event, J_CSTR_TO_BUF("onConnectionStateChange")) &&
               jis_object(values)) {
        pad->connected = json_bool(values, "connected");
    } else if (jis_string(event) && jstring_equal2(event, J_CSTR_TO_BUF("onRequestMtu"))) {
        pad->mtu_done = true;
        commons_log_info("SteamBLE", "Steam Controller %s: MTU exchanged", pad->addr);
    }
    jdomparser_release(&parser);
    return ok;
}

/* Reads what luna-send-pub printed since the last tick; one JSON object per line. */
static void read_client(steam_pad_t *pad) {
    for (;;) {
        ssize_t n = read(pad->conn_fd, pad->line + pad->line_len, sizeof(pad->line) - 1 - pad->line_len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return; /* EAGAIN: nothing more for now */
        }
        if (n == 0) {
            /* luna-send-pub exited: the subscription is gone. */
            close_client(pad, false);
            return;
        }
        pad->line_len += (size_t) n;
        pad->line[pad->line_len] = '\0';
        char *nl;
        while ((nl = strchr(pad->line, '\n')) != NULL) {
            *nl = '\0';
            bool ok = pad->line[0] == '\0' || on_connect_message(pad, pad->line);
            size_t rest = pad->line_len - (size_t) (nl + 1 - pad->line);
            memmove(pad->line, nl + 1, rest + 1);
            pad->line_len = rest;
            if (!ok) {
                close_client(pad, false);
                return;
            }
        }
        if (pad->line_len == sizeof(pad->line) - 1) {
            pad->line_len = 0; /* longer than any message blegatt sends here: drop it */
        }
    }
}

static void step_pad(steam_pad_t *pad, bool scanned) {
    if (pad->conn_pid > 0) {
        read_client(pad);
    }
    if (pad->mtu_done) {
        return;
    }
    if (pad->conn_pid <= 0) {
        if (scanned && pad->attempts < MAX_ATTEMPTS) {
            pad->attempts++;
            open_client(pad);
        }
        return;
    }
    /* requestMtu only once the link is up; before that blegatt does not act on it. */
    if (pad->connected && !pad->mtu_requested) {
        char payload[96], error[128];
        snprintf(payload, sizeof(payload), "{\"address\":\"%s\",\"mtuSize\":%d}", pad->addr, REQUEST_MTU);
        pad->mtu_requested = true;
        if (gatt_call(URI_GATT_REQUEST_MTU, payload, error, sizeof(error))) {
            commons_log_info("SteamBLE", "Steam Controller %s: MTU %d requested", pad->addr, REQUEST_MTU);
        } else {
            commons_log_warn("SteamBLE", "client/requestMtu %s failed (attempt %d): %s", pad->addr, pad->attempts,
                             error);
            /* Start over on the next scan, e.g. after "Other process is already in progress". */
            close_client(pad, true);
        }
    }
}

static void update_pads(webos_steam_ble_state_t *state, const addr_list_t *found) {
    /* Gone: it has to be done again after it reconnects. */
    for (int i = 0; i < MAX_PADS; i++) {
        steam_pad_t *pad = &state->pads[i];
        if (!pad->used || list_contains(found, pad->addr)) {
            continue;
        }
        commons_log_info("SteamBLE", "Steam Controller %s disconnected", pad->addr);
        close_client(pad, true);
        pad->used = false;
    }
    for (int i = 0; i < found->count; i++) {
        steam_pad_t *pad = NULL;
        for (int j = 0; j < MAX_PADS && pad == NULL; j++) {
            if (state->pads[j].used && strcmp(state->pads[j].addr, found->addr[i]) == 0) {
                pad = &state->pads[j];
            }
        }
        for (int j = 0; j < MAX_PADS && pad == NULL; j++) {
            if (!state->pads[j].used) {
                pad = &state->pads[j];
                memset(pad, 0, sizeof(*pad));
                pad->used = true;
                pad->conn_pid = -1;
                pad->conn_fd = -1;
                memcpy(pad->addr, found->addr[i], ADDR_LEN);
                commons_log_info("SteamBLE", "Steam Controller %s connected over Bluetooth", pad->addr);
            }
        }
    }
}

static void *watch_proc(void *arg) {
    webos_steam_ble_state_t *state = arg;
    uint64_t scanned = 0;
    while (!atomic_load(&state->stop)) {
        uint64_t now = now_ms();
        bool scan = scanned == 0 || now - scanned >= SCAN_MS;
        if (scan) {
            scanned = now;
            addr_list_t found = {.count = 0};
            scan_bt_status(&found);
            scan_input_devices(&found);
            update_pads(state, &found);
        }
        for (int i = 0; i < MAX_PADS && !atomic_load(&state->stop); i++) {
            if (state->pads[i].used) {
                step_pad(&state->pads[i], scan);
            }
        }
        usleep(100000);
    }
    for (int i = 0; i < MAX_PADS; i++) {
        if (state->pads[i].used) {
            close_client(&state->pads[i], true);
        }
    }
    atomic_store(&state->finished, true);
    return NULL;
}

webos_steam_ble_state_t *webos_steam_ble_start(void) {
    webos_steam_ble_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    atomic_init(&state->stop, false);
    atomic_init(&state->finished, false);
    int rc = pthread_create(&state->thread, NULL, watch_proc, state);
    if (rc != 0) {
        commons_log_warn("SteamBLE", "watch thread did not start (%d)", rc);
        free(state);
        return NULL;
    }
    return state;
}

void webos_steam_ble_stop(webos_steam_ble_state_t *state) {
    if (state == NULL) {
        return;
    }
    atomic_store(&state->stop, true);
    /* A Luna reply that never comes must not hang app exit. */
    for (int waited = 0; waited < 100 && !atomic_load(&state->finished); waited++) {
        usleep(10000);
    }
    if (atomic_load(&state->finished)) {
        pthread_join(state->thread, NULL);
        free(state);
    } else {
        commons_log_warn("SteamBLE", "watch thread still busy at exit");
        pthread_detach(state->thread);
    }
}
