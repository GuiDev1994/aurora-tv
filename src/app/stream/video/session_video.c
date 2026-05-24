#include "session_video.h"

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stream/session.h"

#include "sps_parser.h"

#include "ui/streaming/streaming.controller.h"
#include "util/bus.h"
#include "logging.h"
#include "ss4s.h"
#include "stream/connection/session_connection.h"
#include "stream/session_priv.h"
#include "app.h"

#include <SDL.h>
#include <assert.h>

// Starting capacity for the decode-unit reassembly buffer. Grows on
// demand to accommodate larger frames (e.g. 4K IDR frames at high
// bitrate routinely exceed 2 MB), capped to keep a malformed stream
// from exhausting memory.
#define DECODER_BUFFER_INITIAL_SIZE (2 * 1024 * 1024)
#define DECODER_BUFFER_MAX_SIZE (32 * 1024 * 1024)

/** Slices hint for pipeline decode while later slices still arrive (high bitrate / 120 Hz). */
#define VDEC_STREAM_SLICES_PER_FRAME 4

/** Clamp presentation offset (ms) when tight sync is on (fixed; no longer user-configurable). */
#define PRESENTATION_OFFSET_MS_MIN (-48)
#define PRESENTATION_OFFSET_MS_MAX 0
#define PRESENTATION_OFFSET_MS_DEFAULT (-12)

/*
 * Minimum interval between IDR (keyframe) requests sent to the host.
 *
 * When Starfish signals BufferFull (SS4S_VIDEO_FEED_REQUEST_KEYFRAME) we would
 * normally request an IDR immediately. However, an IDR frame is 4-8x larger
 * than a P-frame; feeding it back into the decoder fills the buffer again,
 * triggering another immediate IDR request - a runaway feedback loop. Worse,
 * IDR requests travel over the same TCP control channel as gamepad / mouse /
 * keyboard input, so a flood of them directly inflates perceived input
 * latency (observed: ~1 s controller lag at high bitrate with Nvidia P1).
 *
 * Throttling to 1000 ms suppresses the storm. Subsequent in-window BufferFull
 * signals are silently swallowed (counted as networkDroppedFrames); the stream
 * self-heals from the next clean P-frames once the buffer drains.
 */
#define IDR_REQUEST_MIN_INTERVAL_MS 1000

static int vdec_stream_target_fps = 60;

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static unsigned char *buffer = NULL;
static size_t buffer_size = 0;
static int lastFrameNumber;
static unsigned long lastIdrRequestMs = 0;
static struct VIDEO_STATS vdec_temp_stats;
static int vdec_stream_format = 0;
static bool vdec_warned_near_buffer_limit;
VIDEO_STATS vdec_summary_stats;
VIDEO_INFO vdec_stream_info;

static int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags);

static void vdec_delegate_cleanup();

static int vdec_delegate_submit(PDECODE_UNIT decodeUnit);

static void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now);

static void stream_info_parse_size(PDECODE_UNIT decodeUnit, struct VIDEO_INFO *info);

DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks = {
        .setup = vdec_delegate_setup,
        .cleanup = vdec_delegate_cleanup,
        .submitDecodeUnit = vdec_delegate_submit,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void session_video_prepare_stream(void) {
    int caps = CAPABILITY_DIRECT_SUBMIT;
    const bool hevc = app_configuration != NULL && app_configuration->hevc;
    const bool av1 = app_configuration != NULL && app_configuration->av1;
    if (hevc) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
    }
    if (av1) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
    }
    if (hevc || av1) {
        caps |= CAPABILITY_SLICES_PER_FRAME(VDEC_STREAM_SLICES_PER_FRAME);
    }
    if (hevc || av1) {
        commons_log_info("Session", "Video SDP caps: RFI + %u slices/frame (HEVC=%d AV1=%d)",
                         (unsigned) VDEC_STREAM_SLICES_PER_FRAME, hevc ? 1 : 0, av1 ? 1 : 0);
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
    buffer_size = DECODER_BUFFER_INITIAL_SIZE;
    buffer = malloc(buffer_size);
    memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
    memset(&vdec_stream_info, 0, sizeof(vdec_stream_info));
    vdec_stream_format = videoFormat;
    vdec_stream_info.format = video_format_name(videoFormat);
    lastFrameNumber = 0;
    lastIdrRequestMs = 0;
    vdec_stream_target_fps = redrawRate > 0 ? redrawRate : 60;
    vdec_warned_near_buffer_limit = false;

    if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        vdec_stream_info.width = width;
        vdec_stream_info.height = height;
    }

    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    info.frameRateNumerator = vdec_stream_target_fps;
    info.frameRateDenominator = 1;
#if TARGET_WEBOS
    if (session->app->settings.video_tight_sync) {
        info.tightFramePacing = true;
        int off = PRESENTATION_OFFSET_MS_DEFAULT;
        if (off > PRESENTATION_OFFSET_MS_MAX) {
            off = PRESENTATION_OFFSET_MS_MAX;
        } else if (off < PRESENTATION_OFFSET_MS_MIN) {
            off = PRESENTATION_OFFSET_MS_MIN;
        }
        info.presentationOffsetMs = off;
    }
#endif
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
            return CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED;
        default:
            return CALLBACKS_SESSION_ERROR_VDEC_ERROR;
    }
}

void vdec_delegate_cleanup() {
    assert(player != NULL);
    free(buffer);
    buffer = NULL;
    buffer_size = 0;
    SS4S_PlayerVideoClose(player);
    session = NULL;
}

int vdec_delegate_submit(PDECODE_UNIT decodeUnit) {
    if ((size_t) decodeUnit->fullLength > buffer_size) {
        if ((size_t) decodeUnit->fullLength > DECODER_BUFFER_MAX_SIZE) {
            commons_log_error("Session", "Decode unit %d bytes exceeds %zu byte cap, dropping",
                              decodeUnit->fullLength, (size_t) DECODER_BUFFER_MAX_SIZE);
            return DR_NEED_IDR;
        }
        size_t new_size = buffer_size > 0 ? buffer_size : DECODER_BUFFER_INITIAL_SIZE;
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
        // Any frame number greater than m_LastFrameNumber + 1 represents a dropped frame
        vdec_temp_stats.networkDroppedFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        vdec_temp_stats.totalFrames += decodeUnit->frameNumber - (lastFrameNumber + 1);
        lastFrameNumber = decodeUnit->frameNumber;
    }
    unsigned stats_window_ms = streaming_stats_shown() ? 1000u : 2000u;
    if (ticksms - vdec_temp_stats.measurementStartTimestamp > stats_window_ms) {
        vdec_stat_submit(&vdec_temp_stats, ticksms);

        // Move this window into the last window slot and clear it for next window
        memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
        vdec_temp_stats.measurementStartTimestamp = ticksms;
    }

    vdec_temp_stats.receivedFrames++;
    vdec_temp_stats.totalFrames++;
    vdec_temp_stats.receivedBytes += (uint64_t) decodeUnit->fullLength;

    vdec_temp_stats.totalCaptureLatency += decodeUnit->frameHostProcessingLatency;
    vdec_temp_stats.totalReassemblyTime += decodeUnit->enqueueTimeMs - decodeUnit->receiveTimeMs;
    vdec_stream_info.has_host_latency |= decodeUnit->frameHostProcessingLatency > 0;
    if (!vdec_warned_near_buffer_limit &&
        (size_t) decodeUnit->fullLength > (DECODER_BUFFER_INITIAL_SIZE * 9 / 10)) {
        vdec_warned_near_buffer_limit = true;
        commons_log_warn("Session", "Video frame size %d is near initial decoder buffer (%zu)",
                         decodeUnit->fullLength, (size_t) DECODER_BUFFER_INITIAL_SIZE);
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
    SS4S_VideoFeedResult result = SS4S_PlayerVideoFeed(player, buffer, length, flags);
    if (result == SS4S_VIDEO_FEED_OK) {
        if (vdec_stream_info.width == 0 || vdec_stream_info.height == 0) {
            stream_info_parse_size(decodeUnit, &vdec_stream_info);
        }
        vdec_temp_stats.totalSubmitTime += LiGetMillis() - decodeUnit->enqueueTimeMs;
        vdec_temp_stats.submittedFrames++;
        return DR_OK;
    } else if (result == SS4S_VIDEO_FEED_REQUEST_KEYFRAME) {
        unsigned long now = SDL_GetTicks();
        if (now - lastIdrRequestMs >= IDR_REQUEST_MIN_INTERVAL_MS) {
            lastIdrRequestMs = now;
            commons_log_info("Session", "Buffer full, requesting IDR (throttled to %u ms)",
                             (unsigned) IDR_REQUEST_MIN_INTERVAL_MS);
            return DR_NEED_IDR;
        }
        vdec_temp_stats.networkDroppedFrames++;
        return DR_OK;
    } else {
        commons_log_error("Session", "Video feed error %d", result);
        session_interrupt(session, false, STREAMING_INTERRUPT_DECODER);
        return DR_OK;
    }
}

void vdec_stat_submit(const struct VIDEO_STATS *src, unsigned long now) {
    struct VIDEO_STATS *dst = &vdec_summary_stats;
    memcpy(dst, src, sizeof(struct VIDEO_STATS));
    unsigned long delta = now - dst->measurementStartTimestamp;
    if (delta <= 0) { return; }
    dst->totalFps = (float) dst->totalFrames / ((float) delta / 1000);
    dst->receivedFps = (float) dst->receivedFrames / ((float) delta / 1000);
    dst->decodedFps = (float) dst->submittedFrames / ((float) delta / 1000);
    dst->currentBitrateKbps = (uint32_t) ((dst->receivedBytes * 8) / (delta / 1000.0f));
    const bool show_stats = streaming_stats_shown();
    if (show_stats) {
        LiGetEstimatedRttInfo(&dst->rtt, &dst->rttVariance);
    }
    if (!show_stats) {
        return;
    }
    int latencyUs = 0;
    if (SS4S_PlayerGetVideoLatency(player, 0, &latencyUs)) {
        dst->avgDecoderLatency = (float) latencyUs / 1000.0f;
        vdec_stream_info.has_decoder_latency = true;
    } else {
        dst->avgDecoderLatency = 0;
    }
    app_bus_post(session->app, (bus_actionfunc) streaming_refresh_stats, NULL);
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
