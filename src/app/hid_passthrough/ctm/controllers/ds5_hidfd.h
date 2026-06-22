#ifndef DS5_HIDFD_H
#define DS5_HIDFD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow an open O_RDWR fd for a /dev/hidrawN node from the root ds5_txd broker.
 *
 * The webOS app jail's /dev is a STATIC snapshot taken at jail-build, so a
 * controller hot-plugged onto a hidraw minor that snapshot never covered does not
 * exist inside the jail and the app's own open() returns ENOENT. The broker (root,
 * real /dev) opens the node and passes the fd back over AF_UNIX via SCM_RIGHTS.
 *
 * Returns a ready-to-use fd (O_NONBLOCK + CLOEXEC set), or -1 if the broker is
 * absent / declined. Call only as a fallback after a direct open() fails, so the
 * static-node path is never disturbed. Never blocks more than a couple seconds. */
int ds5_hidfd_request(const char *dev_path);

#ifdef __cplusplus
}
#endif

#endif /* DS5_HIDFD_H */
