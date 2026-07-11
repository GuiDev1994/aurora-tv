#include "session_video.h"

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stream/session.h"

#include "app_settings.h"
#include "sps_parser.h"

#include "ui/streaming/streaming.controller.h"
#include "util/bus.h"
#include "logging.h"
#include "ss4s.h"
#include "stream/connection/session_connection.h"
#include "stream/session_priv.h"
#include "stream/adaptive_bitrate.h"
#include "app.h"

#include <SDL.h>
#include <assert.h>

/* Starting capacity for the decode-unit reassembly buffer. Grows on
 * demand to accommodate larger frames (e.g. 4K IDR frames at high
 * bitrate), capped to keep a malformed stream from exhausting memory. */
#define DECODER_BUFFER_MAX_SIZE (32 * 1024 * 1024)

/** Slices hint for pipeline decode while later slices still arrive (high bitrate / 120 Hz). */
#define VDEC_STREAM_SLICES_MIN 4
#define VDEC_STREAM_SLICES_MAX 8

static unsigned vdec_slices_for_stream(int width, int height, int fps) {
    if (fps <= 0) {
        fps = 60;
    }
    const int64_t load = (int64_t) width * (int64_t) height * (int64_t) fps;
    if (load >= (int64_t) 3840 * 2160 * 90) {
        return VDEC_STREAM_SLICES_MAX;
    }
    if (load >= (int64_t) 2560 * 1440 * 90) {
        return 6;
    }
    return VDEC_STREAM_SLICES_MIN;
}

static int vdec_stream_target_fps = 60;

static int frames_since_idr = 0;

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static unsigned char *buffer = NULL;
static size_t buffer_size = 0;
static size_t buffer_initial_size = 0;
static int lastFrameNumber;
/* Set when SS4S_PlayerVideoFeed returns NOT_READY. Consumed on the next
 * successful Feed: ask Limelight for one IDR so the decoder can resync. */
static bool need_idr_on_resume = false;
static struct VIDEO_STATS vdec_temp_stats;
static int vdec_stream_format = 0;
static bool vdec_warned_near_buffer_limit;
/* Set once per stream after the first successful feed, so SS4S_PlayerVideoSetHDRInfo is only
 * called once the video sink actually exists (a no-op on webOS before then). */
static bool vdec_hdr_notified;
VIDEO_STATS vdec_summary_stats;
/* Seqlock for vdec_summary_stats: odd while vdec_stat_submit is mid-write. */
static unsigned vdec_stats_seq;
VIDEO_INFO vdec_stream_info;

#if defined(TARGET_WEBOS)
#define SOFT_REC_SUSTAIN_MS 3000u
#define SOFT_REC_COOLDOWN_MS 20000u
#define SOFT_REC_HOLD_MS 15000
#define SOFT_REC_DROP_PERCENT 75
#define SOFT_REC_RQ_THRESHOLD 2
#define SOFT_REC_LATENCY_MS 80.0f

static Uint32 soft_rec_high_since = 0;
static Uint32 soft_rec_cooldown_until = 0;

static void soft_recovery_reset(void) {
    soft_rec_high_since = 0;
    soft_rec_cooldown_until = 0;
}

static bool soft_recovery_is_4k(void) {
    int w = session != NULL ? session->config.stream.width : 0;
    int h = session != NULL ? session->config.stream.height : 0;
    if (vdec_stream_info.width > 0 && vdec_stream_info.height > 0) {
        w = vdec_stream_info.width;
        h = vdec_stream_info.height;
    }
    return ((int64_t) w * (int64_t) h) >= ((int64_t) 3840 * 2160);
}

static bool soft_recovery_pressure(const struct VIDEO_STATS *dst) {
    if (dst->videoRenderQueue >= SOFT_REC_RQ_THRESHOLD) {
        return true;
    }
    float target = (float) vdec_stream_target_fps;
    if (target < 1.0f) {
        target = 60.0f;
    }
    if (dst->receivedFps >= target * 0.9f && dst->decodedFps < target * 0.85f && dst->submittedFrames > 0) {
        return true;
    }
    if (vdec_stream_info.has_decoder_latency && dst->avgDecoderLatency > SOFT_REC_LATENCY_MS) {
        return true;
    }
    return false;
}

static void soft_recovery_tick(const struct VIDEO_STATS *dst) {
    if (session == NULL || !session->config.soft_recovery || session->abr == NULL) {
        return;
    }
    if (!soft_recovery_is_4k()) {
        soft_rec_high_since = 0;
        return;
    }
    Uint32 now = SDL_GetTicks();
    if (soft_rec_cooldown_until != 0 && now < soft_rec_cooldown_until) {
        return;
    }
    if (!soft_recovery_pressure(dst)) {
        soft_rec_high_since = 0;
        return;
    }
    if (soft_rec_high_since == 0) {
        soft_rec_high_since = now;
        return;
    }
    if (now - soft_rec_high_since < SOFT_REC_SUSTAIN_MS) {
        return;
    }
    if (adaptive_bitrate_request_drop(session->abr, SOFT_REC_DROP_PERCENT, SOFT_REC_HOLD_MS,
                                      "4K decode backlog")) {
        commons_log_info("Session",
                         "Soft recovery: sustained 4K backlog (RQ=%d decodedFps=%.1f latency=%.1fms) "
                         "→ request %d%% bitrate",
                         dst->videoRenderQueue, dst->decodedFps, dst->avgDecoderLatency,
                         SOFT_REC_DROP_PERCENT);
        soft_rec_cooldown_until = now + SOFT_REC_COOLDOWN_MS;
    }
    soft_rec_high_since = 0;
}
#endif

static int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags);

static void vdec_delegate_cleanup(void);

static int vdec_delegate_submit(PDECODE_UNIT decodeUnit);

static int vdec_finish_feed(SS4S_VideoFeedResult result, PDECODE_UNIT decodeUnit);

static void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now);

static void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info);

static size_t vdec_buffer_initial_bytes(void) {
    return (size_t) VDEC_REASSEMBLY_BUFFER_MB * 1024U * 1024U;
}

DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks = {
        .setup = vdec_delegate_setup,
        .cleanup = vdec_delegate_cleanup,
        .submitDecodeUnit = vdec_delegate_submit,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void session_video_prepare_stream(void) {
    int caps = CAPABILITY_DIRECT_SUBMIT;
    const bool hevc = app_configuration != NULL && app_configuration->hevc;
    if (hevc) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
        unsigned slices = VDEC_STREAM_SLICES_MIN;
        if (app_configuration != NULL) {
            slices = vdec_slices_for_stream(app_configuration->stream.width,
                                            app_configuration->stream.height,
                                            app_configuration->stream.fps);
        }
        caps |= CAPABILITY_SLICES_PER_FRAME(slices);
        commons_log_info("Session", "Video SDP caps: RFI + %u slices/frame (HEVC=1)", slices);
    } else {
        commons_log_info("Session", "Video SDP caps: direct submit only (H.264)");
    }
    ss4s_dec_callbacks.capabilities = caps;
}

static const char *video_format_name(int videoFormat) {
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            return "H264";
        case VIDEO_FORMAT_H265:
            return "H265";
        case VIDEO_FORMAT_H265_MAIN10:
            return "H265 10bit";
        case VIDEO_FORMAT_AV1_MAIN8:
            return "AV1 8bit";
        case VIDEO_FORMAT_AV1_MAIN10:
            return "AV1 10bit";
        default:
            if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
                return "AV1";
            }
            return "Unknown";
    }
}

int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
    (void) drFlags;
    session = context;
    player = session->player;
    buffer_initial_size = vdec_buffer_initial_bytes();
    buffer_size = buffer_initial_size;
    buffer = malloc(buffer_size);
    if (!buffer) {
        commons_log_error("Session", "Failed to allocate video reassembly buffer (%zu bytes)", buffer_size);
        return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
    memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
    memset(&vdec_stream_info, 0, sizeof(vdec_stream_info));
    vdec_stream_format = videoFormat;
    vdec_stream_info.format = video_format_name(videoFormat);
    vdec_hdr_notified = false;
    lastFrameNumber = 0;
    need_idr_on_resume = false;
    frames_since_idr = 0;
    vdec_stream_target_fps = redrawRate > 0 ? redrawRate : 60;
    vdec_warned_near_buffer_limit = false;
#if defined(TARGET_WEBOS)
    soft_recovery_reset();
#endif

    if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        vdec_stream_info.width = width;
        vdec_stream_info.height = height;
    }

    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    /* Prefer host fractional refresh (e.g. 11988/100) for Starfish maxFrameRate / smooth
     * pacing so the decoder grid matches NTSC clientRefreshRateX100 when set. */
    if (session != NULL && session->config.stream.clientRefreshRateX100 > 0 &&
        (session->config.stream.clientRefreshRateX100 % 100) != 0) {
        info.frameRateNumerator = session->config.stream.clientRefreshRateX100;
        info.frameRateDenominator = 100;
    } else {
        info.frameRateNumerator = vdec_stream_target_fps;
        info.frameRateDenominator = 1;
    }
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:
            info.codec = SS4S_VIDEO_H264;
            break;
        case VIDEO_FORMAT_H265:
        case VIDEO_FORMAT_H265_MAIN10:
            info.codec = SS4S_VIDEO_H265;
            break;
        case VIDEO_FORMAT_AV1_MAIN8:
        case VIDEO_FORMAT_AV1_MAIN10:
            info.codec = SS4S_VIDEO_AV1;
            break;
        default: {
            commons_log_error("Session", "Unsupported codec %s", vdec_stream_info.format);
            free(buffer);
            buffer = NULL;
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        }
    }

    app_t *app = session->app;
    if (app->ss4s.video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_EXCLUSIVE) {
        app_bus_post_sync(app, (bus_actionfunc) app_ui_close, &app->ui);
    }

    switch (SS4S_PlayerVideoOpen(player, &info)) {
        case SS4S_VIDEO_OPEN_OK: {
            return 0;
        }
        case SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC:
            free(buffer);
            buffer = NULL;
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        default:
            free(buffer);
            buffer = NULL;
            return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
}

void vdec_delegate_cleanup(void) {
    assert(player != NULL);
    free(buffer);
    buffer = NULL;
    buffer_size = 0;
    buffer_initial_size = 0;
#if defined(TARGET_WEBOS)
    soft_recovery_reset();
#endif
    SS4S_PlayerVideoClose(player);
    session = NULL;
}

static int vdec_finish_feed(SS4S_VideoFeedResult result, PDECODE_UNIT decodeUnit) {
    if (result == SS4S_VIDEO_FEED_OK) {
        /* Some GameStream-compatible hosts (e.g. punktfunk) never send the async HDR_INFO
         * control message Sunshine uses to confirm HDR; the negotiated format already tells
         * us HDR was agreed on, so apply it here once the video sink is confirmed alive. */
        if (!vdec_hdr_notified && (vdec_stream_format & VIDEO_FORMAT_MASK_10BIT)) {
            vdec_hdr_notified = true;
            streaming_set_hdr(session, true);
        }
        if (decodeUnit->frameType == FRAME_TYPE_IDR) {
            frames_since_idr = 0;
        } else {
            frames_since_idr++;
        }
        const int idr_ms = app_configuration ? app_configuration->idr_refresh_interval_ms : 0;
        const bool hevc_stream = vdec_stream_format == VIDEO_FORMAT_H265 ||
                                 vdec_stream_format == VIDEO_FORMAT_H265_MAIN10;
        if (hevc_stream && idr_ms >= 500 && vdec_stream_target_fps > 0) {
            const int frames_threshold = (vdec_stream_target_fps * idr_ms + 500) / 1000;
            if (frames_threshold > 0 && frames_since_idr >= frames_threshold) {
                LiRequestIdrFrame();
                frames_since_idr = 0;
            }
        }
        if (vdec_stream_info.width == 0 || vdec_stream_info.height == 0) {
            stream_info_parse_size(decodeUnit, &vdec_stream_info);
        }
        vdec_temp_stats.totalSubmitTime += LiGetMillis() - (unsigned long) (decodeUnit->enqueueTimeUs / 1000);
        vdec_temp_stats.submittedFrames++;
        if (need_idr_on_resume) {
            need_idr_on_resume = false;
            return DR_NEED_IDR;
        }
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_REQUEST_KEYFRAME) {
        return DR_NEED_IDR;
    } else if (result == SS4S_VIDEO_FEED_NOT_READY) {
        need_idr_on_resume = true;
        return DR_OK;
    } else {
        commons_log_error("Session", "Video feed error %d", result);
        session_interrupt(session, false, STREAMING_INTERRUPT_DECODER);
        return DR_OK;
    }
}

int vdec_delegate_submit(PDECODE_UNIT decodeUnit) {
    if ((size_t) decodeUnit->fullLength > buffer_size) {
        if ((size_t) decodeUnit->fullLength > DECODER_BUFFER_MAX_SIZE) {
            commons_log_error("Session", "Decode unit %d bytes exceeds %zu byte cap, dropping",
                              decodeUnit->fullLength, (size_t) DECODER_BUFFER_MAX_SIZE);
            return DR_NEED_IDR;
        }
        size_t new_size = buffer_size > 0 ? buffer_size : buffer_initial_size;
        if (new_size == 0) {
            new_size = vdec_buffer_initial_bytes();
        }
        while (new_size < (size_t) decodeUnit->fullLength) {
            new_size *= 2;
        }
        unsigned char *new_buffer = realloc(buffer, new_size);
        if (new_buffer == NULL) {
            commons_log_error("Session", "Failed to grow decode buffer to %zu bytes", new_size);
            return DR_NEED_IDR;
        }
        commons_log_info("Session", "Grew decode buffer %zu -> %zu bytes (frame needed %d)",
                         buffer_size, new_size, decodeUnit->fullLength);
        buffer = new_buffer;
        buffer_size = new_size;
    }
    unsigned long ticksms = SDL_GetTicks();
    if (lastFrameNumber <= 0) {
        vdec_temp_stats.measurementStartTimestamp = ticksms;
        lastFrameNumber = decodeUnit->frameNumber;
    } else {
        vdec_temp_stats.networkDroppedFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        vdec_temp_stats.totalFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        lastFrameNumber = decodeUnit->frameNumber;
    }
    unsigned stats_window_ms = streaming_stats_shown() ? 1000u : 2000u;
    if (ticksms - vdec_temp_stats.measurementStartTimestamp > stats_window_ms) {
        vdec_stat_submit(&vdec_temp_stats, ticksms);
        memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
        vdec_temp_stats.measurementStartTimestamp = ticksms;
    }

    vdec_temp_stats.receivedFrames++;
    vdec_temp_stats.totalFrames++;
    vdec_temp_stats.receivedBytes += (uint64_t) decodeUnit->fullLength;

    vdec_temp_stats.totalCaptureLatency += decodeUnit->frameHostProcessingLatency;
    vdec_temp_stats.totalReassemblyTime += (uint32_t) ((decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs) / 1000);
    vdec_stream_info.has_host_latency |= decodeUnit->frameHostProcessingLatency > 0;
    if (!vdec_warned_near_buffer_limit && buffer_initial_size > 0 &&
        (size_t) decodeUnit->fullLength > (buffer_initial_size * 9 / 10)) {
        vdec_warned_near_buffer_limit = true;
        commons_log_warn("Session", "Video frame size %d is near initial decoder buffer (%zu)",
                         decodeUnit->fullLength, buffer_initial_size);
    }
    size_t length = 0;
    PLENTRY entry = decodeUnit->bufferList;
    if (entry != NULL && entry->next == NULL) {
        memcpy(buffer, entry->data, entry->length);
        length = (size_t) entry->length;
    } else {
        for (; entry != NULL; entry = entry->next) {
            memcpy(buffer + length, entry->data, entry->length);
            length += entry->length;
        }
    }
    SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
    }
    const int64_t pts_us = decodeUnit->presentationTimeUs > 0
                                   ? (int64_t) decodeUnit->presentationTimeUs
                                   : (int64_t) -1;
    SS4S_VideoFeedResult result = SS4S_PlayerVideoFeedWithPTS(player, buffer, length, flags, pts_us);
    return vdec_finish_feed(result, decodeUnit);
}

static inline void vdec_stats_write_begin(void) {
    vdec_stats_seq++; /* odd: write in progress */
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void vdec_stats_write_end(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vdec_stats_seq++; /* even: consistent */
}

void vdec_stats_snapshot(struct VIDEO_STATS *out) {
    unsigned s1, s2;
    do {
        s1 = __atomic_load_n(&vdec_stats_seq, __ATOMIC_ACQUIRE);
        memcpy(out, &vdec_summary_stats, sizeof(*out));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n(&vdec_stats_seq, __ATOMIC_RELAXED);
    } while ((s1 & 1u) != 0 || s1 != s2);
}

void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now) {
    struct VIDEO_STATS *dst = &vdec_summary_stats;
    vdec_stats_write_begin();
    memcpy(dst, src, sizeof(struct VIDEO_STATS));
    unsigned long delta = now - dst->measurementStartTimestamp;
    if (delta <= 0) {
        vdec_stats_write_end();
        return;
    }
    dst->totalFps = (float) dst->totalFrames / ((float) delta / 1000);
    dst->receivedFps = (float) dst->receivedFrames / ((float) delta / 1000);
    dst->decodedFps = (float) dst->submittedFrames / ((float) delta / 1000);
    dst->currentBitrateKbps = (uint32_t) ((dst->receivedBytes * 8) / (delta / 1000.0f));
    const bool show_stats = streaming_stats_shown();
    if (show_stats) {
        LiGetEstimatedRttInfo(&dst->rtt, &dst->rttVariance);
    }

    /* Always sample latency/RQ so soft recovery works with the overlay hidden. */
    int latencyUS = 0;
    if (player != NULL && SS4S_PlayerGetVideoLatency(player, 0, &latencyUS)) {
        dst->avgDecoderLatency = (float) latencyUS / 1000.0f;
        vdec_stream_info.has_decoder_latency = true;
    } else {
        dst->avgDecoderLatency = 0;
    }
    int rq = -1;
    if (player != NULL && SS4S_PlayerGetVideoRenderQueueLength(player, &rq)) {
        dst->videoRenderQueue = rq;
        vdec_stream_info.has_render_queue = true;
    } else {
        dst->videoRenderQueue = -1;
    }
    vdec_stats_write_end();

#if defined(TARGET_WEBOS)
    soft_recovery_tick(dst);
#endif

    if (show_stats) {
        app_bus_post(session->app, (bus_actionfunc) streaming_refresh_stats, NULL);
    }
}

void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info) {
    if (decodeUnit->frameType != FRAME_TYPE_IDR) { return; }
    if (vdec_stream_format & VIDEO_FORMAT_MASK_AV1) {
        return;
    }
    for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
        if (entry->bufferType != BUFFER_TYPE_SPS) { continue; }
        sps_dimension_t dimension;
        if (vdec_stream_format & VIDEO_FORMAT_MASK_H264) {
            sps_parse_dimension_h264((const unsigned char *) &entry->data[4], &dimension);
        } else if (vdec_stream_format & VIDEO_FORMAT_MASK_H265) {
            sps_parse_dimension_hevc((const unsigned char *) &entry->data[4], &dimension);
        } else {
            info->width = info->height = -1;
            return;
        }
        info->width = dimension.width;
        info->height = dimension.height;
        return;
    }
}
