/* Raw-ACL output for DS5 BT reports — FORWARDER mode. See ds5_acl_tx.h.
 *
 * The webOS app jail lets us bind an HCI_CHANNEL_RAW socket but DENIES the ACL
 * write() (EPERM), so in-process raw-ACL injection is impossible. Instead a root
 * daemon (ds5_txd) does the privileged write: it captures the connection template
 * via HCI_CHANNEL_MONITOR (root), publishes a readiness file, and injects each
 * report we hand it over a local AF_UNIX SOCK_DGRAM socket. We therefore touch no
 * HCI here at all — only a local datagram send (~tens of microseconds, no
 * buffering, no round trip; far below the ~10ms audio grid) plus a poll of the
 * readiness file. Until the daemon signals it has a template, we return HIDRAW so
 * the caller seeds the report on /dev/hidraw (which puts it on air for the daemon
 * to capture the template from). If the daemon is absent, everything degrades to
 * the normal hidraw path — no regression.
 */

#define _GNU_SOURCE

#include "ds5_acl_tx.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ACL_MAX_REPORT 4096

struct ds5_acl_tx {
    ds5_acl_log_fn log_fn;      /* routes milestones to the controller file log */
    void     *log_ctx;
    int       unixfd;           /* AF_UNIX SOCK_DGRAM to the root daemon */
    struct sockaddr_un daddr;   /* daemon socket address (path-based) */
    char      tmpl_path[256];   /* readiness/template file the daemon publishes */

    pthread_t poll_thread;
    int       poll_started;
    volatile int running;
    volatile int ready;         /* daemon has a live template -> safe to forward */

    long      injected;         /* reports handed to the daemon (== on-air injects) */
    long      dropped;          /* transient send congestion */
};

/* Route a milestone to the controller log (if set) or stderr. */
static void acl_log(ds5_acl_tx_t *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (t && t->log_fn) t->log_fn(t->log_ctx, buf);
    else fprintf(stderr, "[ds5-acl] %s\n", buf);
}

/* Readiness poller: the root daemon publishes a 16-byte record
 * ['C''T''M''T'][ver=1][flags][nonce LE16][acl_hdr 8]; flags bit0 = valid (the
 * daemon has captured the current connection's template and can inject). We only
 * need the valid bit: ready=1 => forward to the daemon; ready=0 => seed via
 * hidraw so the daemon can capture the template from on-air traffic. */
static void *acl_mon_thread(void *arg)
{
    ds5_acl_tx_t *t = (ds5_acl_tx_t *)arg;
    acl_log(t, "forwarder readiness watching %s", t->tmpl_path);
    int last = -1;
    while (t->running) {
        int valid = 0;
        int fd = open(t->tmpl_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            uint8_t rec[16];
            ssize_t n = read(fd, rec, sizeof rec);
            close(fd);
            if (n == 16 && rec[0] == 'D' && rec[1] == 'S' && rec[2] == '5' && rec[3] == 'T' &&
                rec[4] == 1 && (rec[5] & 1))
                valid = 1;
        }
        if (valid != last) {
            __atomic_store_n(&t->ready, valid, __ATOMIC_RELEASE);
            acl_log(t, valid ? "daemon template ready -> raw-ACL forward ACTIVE"
                             : "daemon template not ready -> hidraw seeding");
            last = valid;
        }
        for (int i = 0; i < 4 && t->running; i++) usleep(50000);   /* ~200ms poll */
    }
    return NULL;
}

ds5_acl_tx_t *ds5_acl_tx_start(int hci_dev, ds5_acl_log_fn log_fn, void *log_ctx)
{
    (void)hci_dev;
    ds5_acl_tx_t *t = (ds5_acl_tx_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->log_fn = log_fn;
    t->log_ctx = log_ctx;
    t->unixfd = -1;
    t->running = 1;

    const char *tp = getenv("DS5_ACL_TMPL");
    snprintf(t->tmpl_path, sizeof t->tmpl_path, "%s", (tp && tp[0]) ? tp : "/tmp/ds5_acl_tmpl");
    const char *sp = getenv("DS5_ACL_SOCK");
    const char *sock = (sp && sp[0]) ? sp : "/tmp/ds5_acl.sock";

    t->unixfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (t->unixfd < 0) {
        acl_log(t, "unix socket failed errno=%d (staying on hidraw)", errno);
        free(t);
        return NULL;
    }
    int fl = fcntl(t->unixfd, F_GETFL, 0);
    if (fl >= 0) fcntl(t->unixfd, F_SETFL, fl | O_NONBLOCK);   /* never stall the session thread */
    t->daddr.sun_family = AF_UNIX;
    snprintf(t->daddr.sun_path, sizeof t->daddr.sun_path, "%s", sock);

    if (pthread_create(&t->poll_thread, NULL, acl_mon_thread, t) != 0) {
        acl_log(t, "readiness thread failed errno=%d (staying on hidraw)", errno);
        close(t->unixfd);
        free(t);
        return NULL;
    }
    t->poll_started = 1;
    acl_log(t, "raw-ACL forwarder ON: sock=%s (root ds5_txd does the inject)", sock);
    return t;
}

int ds5_acl_tx_send(ds5_acl_tx_t *t, const uint8_t *report, size_t len)
{
    if (!t) return DS5_ACL_TX_HIDRAW;
    if (!report || len == 0 || len > ACL_MAX_REPORT) return DS5_ACL_TX_HIDRAW;
    if (!__atomic_load_n(&t->ready, __ATOMIC_ACQUIRE)) return DS5_ACL_TX_HIDRAW;

    ssize_t wr = sendto(t->unixfd, report, len, 0,
                        (struct sockaddr *)&t->daddr, sizeof t->daddr);
    if (wr == (ssize_t)len) {
        t->injected++;
        return DS5_ACL_TX_SENT;
    }
    if (wr < 0 && (errno == EINTR || errno == EAGAIN ||
                   errno == EWOULDBLOCK || errno == ENOBUFS)) {
        t->dropped++;                 /* daemon recv buffer momentarily full — skip */
        return DS5_ACL_TX_DROP;
    }
    /* Daemon gone / socket missing (ENOENT, ECONNREFUSED): fall back to hidraw for
     * this frame. NOT permanent — the daemon may come back and the readiness poll
     * will re-enable forwarding. */
    return DS5_ACL_TX_HIDRAW;
}

void ds5_acl_tx_stats(ds5_acl_tx_t *t, long *injected, long *dropped, int *ready)
{
    if (!t) {
        if (injected) *injected = 0;
        if (dropped) *dropped = 0;
        if (ready) *ready = 0;
        return;
    }
    if (injected) *injected = t->injected;
    if (dropped) *dropped = t->dropped;
    if (ready) *ready = __atomic_load_n(&t->ready, __ATOMIC_ACQUIRE);
}

void ds5_acl_tx_stop(ds5_acl_tx_t *t)
{
    if (!t) return;
    t->running = 0;
    if (t->poll_started) {
        pthread_join(t->poll_thread, NULL);
        t->poll_started = 0;
    }
    if (t->unixfd >= 0) { close(t->unixfd); t->unixfd = -1; }
    free(t);
}
