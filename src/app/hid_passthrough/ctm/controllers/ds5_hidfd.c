/* HID-FD broker client — see ds5_hidfd.h. */

#define _GNU_SOURCE

#include "ds5_hidfd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int ds5_hidfd_request(const char *dev_path)
{
    if (!dev_path || !dev_path[0]) {
        return -1;
    }

    const char *sp = getenv("DS5_HIDFD_SOCK");
    const char *sock = (sp && sp[0]) ? sp : "/tmp/ds5_hidfd.sock";

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        close(s);
        return -1;
    }

    char req[80];
    int len = snprintf(req, sizeof req, "%s\n", dev_path);
    if (len <= 0 || write(s, req, (size_t)len) != len) {
        close(s);
        return -1;
    }

    char status = 0;
    struct iovec iov = { .iov_base = &status, .iov_len = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;

    ssize_t n = recvmsg(s, &msg, 0);
    int fd = -1;
    if (n >= 1 && status == 'O' && !(msg.msg_flags & MSG_CTRUNC)) {
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
                cm->cmsg_len == CMSG_LEN(sizeof(int))) {
                memcpy(&fd, CMSG_DATA(cm), sizeof(int));
                break;
            }
        }
    }
    close(s);

    if (fd >= 0) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}
