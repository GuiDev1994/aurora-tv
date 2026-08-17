#include "stream_priority.h"

#include "game_mode.h"
#include "logging.h"
#include "lunasynccall.h"

#include <pbnjson.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#define URI_HBCHANNEL_EXEC "luna://org.webosbrew.hbchannel.service/exec"

struct webos_stream_priority_state {
    int old_nice;
    char old_governor[32];
    bool governor_changed;
    bool locked_pages;
    int dma_latency_fd;
    int old_rmem_max;
    int old_rmem_default;
    int old_backlog;
    bool net_tuned;
    bool usb_hub_tuned;
};

static bool json_return_value_true(const char *json) {
    if (json == NULL) {
        return false;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema, 0);
    if (parser == NULL || !jdomparser_feed(parser, json, (int) strlen(json)) || !jdomparser_end(parser)) {
        if (parser) {
            jdomparser_release(&parser);
        }
        return false;
    }
    jvalue_ref root = jdomparser_get_result(parser);
    jvalue_ref rv = jobject_get(root, J_CSTR_TO_BUF("returnValue"));
    bool ok = false;
    if (jis_boolean(rv)) {
        jboolean_get(rv, &ok);
    }
    jdomparser_release(&parser);
    return ok;
}

static char *jstring_dup(jvalue_ref v) {
    if (!jis_string(v)) {
        return NULL;
    }
    raw_buffer buf = jstring_get(v);
    if (buf.m_str == NULL) {
        return NULL;
    }
    return strndup(buf.m_str, buf.m_len);
}

static char *exec_root_shell(const char *script) {
    jvalue_ref payload = jobject_create_var(
            jkeyval(J_CSTR_TO_JVAL("command"), jstring_create(script)),
            J_END_OBJ_DECL);
    if (!jis_valid(payload)) {
        return NULL;
    }
    const char *payload_str = jvalue_tostring_simple(payload);
    char *reply = NULL;
    bool called = HLunaServiceCallSync(URI_HBCHANNEL_EXEC, payload_str, true, &reply);
    j_release(&payload);
    if (!called || reply == NULL) {
        free(reply);
        return NULL;
    }
    if (!json_return_value_true(reply)) {
        commons_log_warn("StreamPrio", "hbchannel exec rejected: %s", reply);
        free(reply);
        return NULL;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    jdomparser_ref parser = jdomparser_create(&schema, 0);
    char *stdout_s = NULL;
    if (parser && jdomparser_feed(parser, reply, (int) strlen(reply)) && jdomparser_end(parser)) {
        jvalue_ref root = jdomparser_get_result(parser);
        stdout_s = jstring_dup(jobject_get(root, J_CSTR_TO_BUF("stdoutString")));
    }
    if (parser) {
        jdomparser_release(&parser);
    }
    free(reply);
    return stdout_s;
}

static void trim_newline(char *s) {
    if (s == NULL) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static void apply_cpu_governor(webos_stream_priority_state_t *st) {
    char *cur = exec_root_shell("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null");
    if (cur != NULL) {
        trim_newline(cur);
        if (cur[0] != '\0') {
            strncpy(st->old_governor, cur, sizeof(st->old_governor) - 1);
        }
        free(cur);
    }
    char *out = exec_root_shell(
            "for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do "
            "[ -w \"$g\" ] && echo performance > \"$g\"; done; "
            "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null");
    if (out == NULL) {
        commons_log_warn("StreamPrio", "CPU governor set failed");
        return;
    }
    trim_newline(out);
    st->governor_changed = (st->old_governor[0] != '\0' && strcmp(st->old_governor, "performance") != 0);
    commons_log_info("StreamPrio", "CPU governor -> %s (was %s)",
                     out[0] ? out : "(unknown)",
                     st->old_governor[0] ? st->old_governor : "(unknown)");
    free(out);
}

static bool governor_name_safe(const char *s) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static void restore_cpu_governor(webos_stream_priority_state_t *st) {
    if (!st->governor_changed || !governor_name_safe(st->old_governor)) {
        return;
    }
    char cmd[192];
    int n = snprintf(cmd, sizeof(cmd),
                     "for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do "
                     "[ -w \"$g\" ] && echo %s > \"$g\"; done",
                     st->old_governor);
    if (n < 0 || (size_t) n >= sizeof(cmd)) {
        return;
    }
    char *out = exec_root_shell(cmd);
    free(out);
    commons_log_info("StreamPrio", "restored CPU governor %s", st->old_governor);
}

static int read_sysctl_int(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) {
        v = -1;
    }
    fclose(f);
    return v;
}

static void apply_net_tune(webos_stream_priority_state_t *st) {
    st->old_rmem_max = read_sysctl_int("/proc/sys/net/core/rmem_max");
    st->old_rmem_default = read_sysctl_int("/proc/sys/net/core/rmem_default");
    st->old_backlog = read_sysctl_int("/proc/sys/net/core/netdev_max_backlog");
    /* 512 KiB rmem_max is ~16 ms at 250 Mbps — too small for USB2 bulk bursts. */
    char *out = exec_root_shell(
            "echo 4194304 > /proc/sys/net/core/rmem_max; "
            "echo 1048576 > /proc/sys/net/core/rmem_default; "
            "echo 4194304 > /proc/sys/net/core/wmem_max; "
            "echo 5000 > /proc/sys/net/core/netdev_max_backlog; "
            "cat /proc/sys/net/core/rmem_max");
    if (out == NULL) {
        commons_log_warn("StreamPrio", "net buffer tune failed");
        return;
    }
    trim_newline(out);
    st->net_tuned = true;
    commons_log_info("StreamPrio", "rmem_max %d -> %s", st->old_rmem_max, out);
    free(out);
}

static void restore_net_tune(webos_stream_priority_state_t *st) {
    if (!st->net_tuned) {
        return;
    }
    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd),
                     "echo %d > /proc/sys/net/core/rmem_max; "
                     "echo %d > /proc/sys/net/core/rmem_default; "
                     "echo %d > /proc/sys/net/core/netdev_max_backlog",
                     st->old_rmem_max > 0 ? st->old_rmem_max : 524288,
                     st->old_rmem_default > 0 ? st->old_rmem_default : 212992,
                     st->old_backlog > 0 ? st->old_backlog : 1000);
    if (n < 0 || (size_t) n >= sizeof(cmd)) {
        return;
    }
    char *out = exec_root_shell(cmd);
    free(out);
}

static void apply_usb_hub_on(webos_stream_priority_state_t *st) {
    /* Keep the USB2 hub that hosts r8152 fully powered (bus autosuspend=auto). */
    char *out = exec_root_shell(
            "for d in /sys/bus/usb/devices/*; do "
            "v=$(cat \"$d/idVendor\" 2>/dev/null); p=$(cat \"$d/idProduct\" 2>/dev/null); "
            "[ \"$v\" = 0bda ] && [ \"$p\" = 8153 ] || continue; "
            "echo on > \"$d/power/control\" 2>/dev/null; "
            "hub=$(dirname \"$d\"); "
            "echo on > \"$hub/power/control\" 2>/dev/null; "
            "bus=$(cat \"$d/busnum\" 2>/dev/null); "
            "[ -n \"$bus\" ] && echo on > /sys/bus/usb/devices/usb$bus/power/control 2>/dev/null; "
            "done; echo ok");
    if (out == NULL) {
        return;
    }
    st->usb_hub_tuned = true;
    commons_log_info("StreamPrio", "USB r8152 / hub power=on");
    free(out);
}

static void restore_usb_hub(webos_stream_priority_state_t *st) {
    if (!st->usb_hub_tuned) {
        return;
    }
    char *out = exec_root_shell(
            "for d in /sys/bus/usb/devices/usb*; do "
            "echo auto > \"$d/power/control\" 2>/dev/null; done");
    free(out);
}

static void hold_cpu_dma_latency(webos_stream_priority_state_t *st) {
    st->dma_latency_fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (st->dma_latency_fd < 0) {
        return;
    }
    uint32_t latency = 0;
    if (write(st->dma_latency_fd, &latency, sizeof(latency)) != (ssize_t) sizeof(latency)) {
        close(st->dma_latency_fd);
        st->dma_latency_fd = -1;
        return;
    }
    commons_log_info("StreamPrio", "cpu_dma_latency=0 (block deep C-states)");
}

webos_stream_priority_state_t *webos_stream_priority_enter(void) {
    if (!webos_game_mode_is_rooted()) {
        return NULL;
    }
    webos_stream_priority_state_t *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->dma_latency_fd = -1;

    errno = 0;
    st->old_nice = getpriority(PRIO_PROCESS, 0);
    if (errno != 0) {
        st->old_nice = 0;
    }
    if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
        commons_log_warn("StreamPrio", "setpriority(-10) failed: %s", strerror(errno));
    } else {
        commons_log_info("StreamPrio", "nice %d -> -10", st->old_nice);
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        st->locked_pages = true;
        commons_log_info("StreamPrio", "mlockall current+future");
    } else {
        commons_log_warn("StreamPrio", "mlockall failed: %s", strerror(errno));
    }

    hold_cpu_dma_latency(st);
    apply_cpu_governor(st);
    apply_net_tune(st);
    apply_usb_hub_on(st);

    commons_log_info("StreamPrio", "session boost on (no SCHED_FIFO)");
    return st;
}

void webos_stream_priority_leave(webos_stream_priority_state_t *state) {
    if (state == NULL) {
        return;
    }
    restore_cpu_governor(state);
    restore_net_tune(state);
    restore_usb_hub(state);
    if (state->dma_latency_fd >= 0) {
        close(state->dma_latency_fd);
        state->dma_latency_fd = -1;
    }
    if (state->locked_pages) {
        (void) munlockall();
    }
    if (setpriority(PRIO_PROCESS, 0, state->old_nice) != 0) {
        commons_log_warn("StreamPrio", "restore nice failed: %s", strerror(errno));
    } else {
        commons_log_info("StreamPrio", "restored nice=%d", state->old_nice);
    }
    free(state);
}
