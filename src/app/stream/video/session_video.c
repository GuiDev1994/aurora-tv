#include "session_video.h"

#include "config.h"

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

// 2MB decode size should be fairly enough for everything
#define DECODER_BUFFER_SIZE (2048 * 1024)

/** Min interval between IDR requests (AV1): avoids control-channel storms vs. gamepad input. */
#define VDEC_IDR_REQUEST_MIN_INTERVAL_MS 1000

/** Slices hint for pipeline decode while later slices still arrive (high bitrate / 120 Hz). */
#define VDEC_AV1_SLICES_PER_FRAME 4

/** webOS tight sync: small negative PTS shift (ms) toward panel vsync. */
#define TIGHT_SYNC_PRESENTATION_OFFSET_MS (-12)

static int vdec_stream_target_fps = 60;

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static unsigned char *buffer = NULL;
static int lastFrameNumber;
static unsigned long last_idr_request_ms;
static bool vdec_throttle_idr_requests;
static struct VIDEO_STATS vdec_temp_stats;
static int vdec_stream_format = 0;
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

void session_video_prepare_stream(bool av1_enabled) {
    int caps = CAPABILITY_DIRECT_SUBMIT;
    if (av1_enabled) {
        caps |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
        caps |= CAPABILITY_SLICES_PER_FRAME(VDEC_AV1_SLICES_PER_FRAME);
        commons_log_info("Session", "AV1 streaming option: RFI + %u slices/frame for SDP negotiation",
                         (unsigned) VDEC_AV1_SLICES_PER_FRAME);
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
            return "Unknown";
    }
}

int vdec_delegate_setup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
    (void) drFlags;
    session = context;
    player = session->player;
    buffer = malloc(DECODER_BUFFER_SIZE);
    memset(&vdec_temp_stats, 0, sizeof(vdec_temp_stats));
    memset(&vdec_stream_info, 0, sizeof(vdec_stream_info));
    vdec_stream_format = videoFormat;
    vdec_stream_info.format = video_format_name(videoFormat);
    lastFrameNumber = 0;
    last_idr_request_ms = 0;
    vdec_throttle_idr_requests = (videoFormat & VIDEO_FORMAT_MASK_AV1) != 0;
    vdec_stream_target_fps = redrawRate > 0 ? redrawRate : 60;

    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    info.frameRateNumerator = vdec_stream_target_fps;
    info.frameRateDenominator = 1;
#if TARGET_WEBOS
    if (session->app->settings.video_tight_sync) {
        info.tightFramePacing = true;
        info.presentationOffsetMs = TIGHT_SYNC_PRESENTATION_OFFSET_MS;
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
    SS4S_PlayerVideoClose(player);
    session = NULL;
}

int vdec_delegate_submit(PDECODE_UNIT decodeUnit) {
    if (decodeUnit->fullLength > DECODER_BUFFER_SIZE) {
        return 0;
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
    // Flip stats windows roughly every second
    if (ticksms - vdec_temp_stats.measurementStartTimestamp > 1000) {
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
    size_t length = 0;
    for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
        memcpy(buffer + length, entry->data, entry->length);
        length += entry->length;
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
        if (vdec_throttle_idr_requests) {
            unsigned long now = SDL_GetTicks();
            if (now - last_idr_request_ms < VDEC_IDR_REQUEST_MIN_INTERVAL_MS) {
                return DR_OK;
            }
            last_idr_request_ms = now;
        }
        return DR_NEED_IDR;
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
    LiGetEstimatedRttInfo(&dst->rtt, &dst->rttVariance);
    if (!streaming_stats_shown()) {
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