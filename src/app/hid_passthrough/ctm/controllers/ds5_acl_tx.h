#ifndef DS5_ACL_TX_H
#define DS5_ACL_TX_H

/* Raw HCI-ACL output injector for DS5 BT 0x36 reports.
 *
 * webOS' webos-bluetooth-service paces HID output one-outstanding (writes one
 * ACL, waits ~30-40 ms for completion, then the next) -> the 100/s DS5 audio /
 * voice-coil 0x36 stream lands on air at only ~62/s with 30-67 ms jitter gaps,
 * which is what chops the haptics/controller-audio. This injector writes the
 * already-signed 0x36 report straight to the controller as a raw HCI ACL packet
 * on a second HCI_CHANNEL_RAW socket, bypassing that flow control entirely.
 * Measured on the proven ds5_av_play path: 100/s on air, mean 10 ms, gaps>15=0.
 *
 * Non-invasive + reversible: no kernel/daemon patch. Any failure degrades to the
 * normal /dev/hidraw write. Restricted (Scope A) to the high-rate 0x36 report;
 * the caller leaves all other reports (0x31 rumble/trigger/LED) on hidraw.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct ds5_acl_tx ds5_acl_tx_t;

/* DS5 BT HID output report IDs eligible for raw-ACL injection (Scope B):
 * 0x31 = rumble/trigger/LED, 0x32 + 0x36 = audio/haptic variants. All ride the
 * same L2CAP HID-interrupt channel, so one captured handle/CID template serves
 * every ID (lengths are recomputed per send). Other IDs stay on hidraw. */
static inline int ds5_acl_is_injectable(unsigned char report_id)
{
    return report_id == 0x31 || report_id == 0x32 || report_id == 0x36;
}

/* ds5_acl_tx_send() return codes. */
#define DS5_ACL_TX_SENT    0    /* injected — do NOT also write hidraw */
#define DS5_ACL_TX_HIDRAW  1    /* not ready / disabled — caller MUST write hidraw
                                 * (the hidraw write also seeds template capture) */
#define DS5_ACL_TX_DROP  (-1)   /* transient congestion — skip this report */

/* Optional log sink so the injector's internal milestones (monitor bind result,
 * template capture, fatal write) reach the per-controller file log instead of
 * only stderr (which the webOS jail swallows). msg has no trailing newline. */
typedef void (*ds5_acl_log_fn)(void *ctx, const char *msg);

/* Open an HCI_CHANNEL_RAW socket on hci_dev (0 on the TV) and start a monitor
 * thread that captures the on-air ACL/L2CAP framing of our own outgoing 0x36
 * reports. log_fn (may be NULL) receives diagnostic lines. Returns NULL on any
 * failure (caller stays on hidraw). */
ds5_acl_tx_t *ds5_acl_tx_start(int hci_dev, ds5_acl_log_fn log_fn, void *log_ctx);

/* Inject one already-signed HID output report (report[0] = report id). See the
 * DS5_ACL_TX_* codes for the contract. */
int ds5_acl_tx_send(ds5_acl_tx_t *t, const uint8_t *report, size_t len);

/* Snapshot counters (any pointer may be NULL). */
void ds5_acl_tx_stats(ds5_acl_tx_t *t, long *injected, long *dropped, int *ready);

/* Stop the monitor thread, close the socket, free. */
void ds5_acl_tx_stop(ds5_acl_tx_t *t);

#endif /* DS5_ACL_TX_H */
