#pragma once

#include <Limelight.h>

extern struct VIDEO_STATS vdec_summary_stats;
extern struct VIDEO_INFO vdec_stream_info;
extern struct AUDIO_INFO audio_stream_info;

extern DECODER_RENDERER_CALLBACKS ss4s_dec_callbacks;

/** Tear-free copy of vdec_summary_stats for cross-thread readers (seqlock retry). */
void vdec_stats_snapshot(struct VIDEO_STATS *out);

/** Call before LiStartConnection. Sets decoder capabilities from settings (RFI; optional slices). */
void session_video_prepare_stream(void);

