#pragma once

#include <stdbool.h>

#include <Limelight.h>

extern struct VIDEO_STATS vdec_summary_stats;
extern struct VIDEO_INFO vdec_stream_info;
extern struct AUDIO_INFO audio_stream_info;

extern DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks;

/** Call before LiStartConnection. When @a av1_enabled, negotiates AV1-friendly SDP (RFI + slices). */
void session_video_prepare_stream(bool av1_enabled);

