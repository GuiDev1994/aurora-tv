#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <opus_multistream.h>
#include <Limelight.h>

#include "ss4s.h"
#include "stream/connection/session_connection.h"
#include "stream/session_priv.h"
#include "logging.h"
#include "config.h"
#include "app.h"
#include "app_settings.h"
#include "util/i18n.h"

#define SAMPLES_PER_FRAME  240

static session_t *session = NULL;
static SS4S_Player *player = NULL;
static OpusMSDecoder *decoder = NULL;
static unsigned char *buffer = NULL;
static int frame_size = 0, unit_size = 0;
static uint64_t aud_last_feed_us = 0;

AUDIO_INFO audio_stream_info;

static size_t opus_head_serialize(const OPUS_MULTISTREAM_CONFIGURATION *config, unsigned char *data);

static void aud_buffers_free(void);

static int aud_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void *context,
                    int arFlags) {
    (void) audioConfiguration;
    (void) arFlags;
    memset(&audio_stream_info, 0, sizeof(audio_stream_info));
    aud_last_feed_us = 0;
    session = context;
    player = session->player;
    SS4S_AudioCodec codec = SS4S_AUDIO_PCM_S16LE;
    size_t codecDataLen = 0;
    SS4S_AudioInfo info = {
            .numOfChannels = opusConfig->channelCount,
            .appName = "Aurora",
            .streamName = "Streaming",
            .sampleRate = opusConfig->sampleRate,
            .samplesPerFrame = SAMPLES_PER_FRAME,
    };
    if (opusConfig->channelCount == 6) {
        commons_log_info("Session", "Opus 5.1 from host: streams=%d coupled=%d mapping=[%u,%u,%u,%u,%u,%u]",
                         opusConfig->streams, opusConfig->coupledStreams,
                         opusConfig->mapping[0], opusConfig->mapping[1], opusConfig->mapping[2],
                         opusConfig->mapping[3], opusConfig->mapping[4], opusConfig->mapping[5]);
    }
    int pcm_layout = SURROUND_51_PASSTHROUGH;
#if TARGET_WEBOS
    /* Opt-in escape hatch (#80): NDL Opus 5.1 cuts out on some eARC bars while PCM does not.
     * Passthrough stays the default because it is the verified 5.1 speaker mapping. */
    if (opusConfig->channelCount == 6 && app_configuration != NULL) {
        pcm_layout = app_configuration->surround_51_mode;
    }
#endif
    if (pcm_layout == SURROUND_51_PASSTHROUGH && session->audio_cap.codecs & SS4S_AUDIO_OPUS &&
        SS4S_GetAudioPreferredCodecs(&info) & SS4S_AUDIO_OPUS) {
        codec = SS4S_AUDIO_OPUS;
        decoder = NULL;
        buffer = calloc(1024, sizeof(unsigned char));
        if (buffer == NULL) {
            commons_log_error("Session", "Audio init: failed to allocate codec data buffer");
            return -1;
        }
        codecDataLen = opus_head_serialize(opusConfig, buffer);
    } else {
        int rc;
        /* Decoding with the host's own mapping yields WAVE order (FL FR C LFE SL SR) whatever
         * mapping the host chose. Output channel i is mapping[i], so permuting the mapping
         * reorders the PCM for free. */
        unsigned char mapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];
        memcpy(mapping, opusConfig->mapping, sizeof(mapping));
        if (pcm_layout == SURROUND_51_PCM) {
            /* NDL slots FL RR FR RL C LFE, from WAVE FL(0) FR(1) C(2) LFE(3) RL(4) RR(5). */
            mapping[0] = opusConfig->mapping[0];
            mapping[1] = opusConfig->mapping[5];
            mapping[2] = opusConfig->mapping[1];
            mapping[3] = opusConfig->mapping[4];
            mapping[4] = opusConfig->mapping[2];
            mapping[5] = opusConfig->mapping[3];
            commons_log_info("Session", "5.1 decoded in client, PCM slots FL RR FR RL C LFE");
        }
        decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams,
                                                  opusConfig->coupledStreams, mapping, &rc);
        if (rc != 0) {
            return rc;
        }
        frame_size = opusConfig->samplesPerFrame;
        unit_size = (int) (opusConfig->channelCount * sizeof(int16_t));
        buffer = calloc(unit_size, frame_size);
        if (buffer == NULL) {
            commons_log_error("Session", "Audio init: failed to allocate decode buffer");
            aud_buffers_free();
            return -1;
        }
    }
    audio_stream_info.format = SS4S_AudioCodecName(codec);
    switch (audioConfiguration) {
        case AUDIO_CONFIGURATION_STEREO:
            audio_stream_info.channels = locstr("Stereo");
            break;
        case AUDIO_CONFIGURATION_51_SURROUND:
            audio_stream_info.channels = "5.1ch";
            break;
        case AUDIO_CONFIGURATION_71_SURROUND:
            audio_stream_info.channels = "7.1ch";
            break;
        default:
            audio_stream_info.channels = locstr("Unknown");
            break;
    }
    info.codec = codec;
    info.codecData = buffer;
    info.codecDataLen = codecDataLen;
    // aud_cleanup only runs once the audio stream has started, so a failed init has to release
    // what it allocated itself.
    SS4S_AudioOpenResult result = SS4S_PlayerAudioOpen(player, &info);
    if (result != SS4S_AUDIO_OPEN_OK) {
        aud_buffers_free();
    }
    return result;
}

static void aud_cleanup() {
    if (player != NULL) {
        SS4S_PlayerAudioClose(player);
        player = NULL;
    }
    aud_buffers_free();
    session = NULL;
}

static void aud_buffers_free(void) {
    if (decoder != NULL) {
        opus_multistream_decoder_destroy(decoder);
        decoder = NULL;
    }
    free(buffer);
    buffer = NULL;
}

static void aud_feed(char *sampleData, int sampleLength) {
    uint64_t now_us = LiGetMicroseconds();
    if (aud_last_feed_us != 0) {
        uint32_t gap_ms = (uint32_t) ((now_us - aud_last_feed_us) / 1000ULL);
        if (gap_ms > audio_stream_info.maxGapMs) {
            audio_stream_info.maxGapMs = gap_ms;
        }
        /* Host silence / packet loss: Opus frames are 5 ms; >40 ms is a real gap. */
        if (gap_ms >= 40 && gap_ms == audio_stream_info.maxGapMs) {
            commons_log_warn("Session", "Audio inter-feed gap %u ms (new max)",
                             (unsigned) gap_ms);
        }
    }
    aud_last_feed_us = now_us;

    SS4S_AudioFeedResult result;
    if (decoder != NULL) {
        int decode_len = opus_multistream_decode(decoder, (unsigned char *) sampleData, sampleLength,
                                                 (opus_int16 *) buffer, frame_size, 0);
        // Negative return = decode error (e.g. corrupt packet); feeding
        // unit_size * decode_len would pass a huge wrapped size downstream.
        if (decode_len <= 0) {
            audio_stream_info.feedFailures++;
            return;
        }
        result = SS4S_PlayerAudioFeed(player, buffer, unit_size * decode_len);
    } else {
        result = SS4S_PlayerAudioFeed(player, (unsigned char *) sampleData, sampleLength);
    }
    if (result != SS4S_AUDIO_FEED_OK) {
        audio_stream_info.feedFailures++;
        /* Rate-limit: first failure and every 50th thereafter. */
        if (audio_stream_info.feedFailures == 1 || (audio_stream_info.feedFailures % 50) == 0) {
            commons_log_warn("Session", "Audio feed failure #%u result=%d",
                             (unsigned) audio_stream_info.feedFailures, (int) result);
        }
    }
}

static size_t opus_head_serialize(const OPUS_MULTISTREAM_CONFIGURATION *config, unsigned char *data) {
    unsigned char *ptr = data;
    // 1. Magic Signature:
    ptr = memcpy(ptr, "OpusHead", 8) + 8;
    // 2. Version (8 bits, unsigned):
    *ptr++ = 1;
    // 3. Output Channel Count 'C' (8 bits, unsigned):
    *ptr++ = config->channelCount;
    // 4. Pre-skip (16 bits, unsigned, little-endian):
    uint16_t preskip = SDL_SwapLE16(0);
    ptr = memcpy(ptr, &preskip, 2) + 2;
    // 5. Input Sample Rate (32 bits, unsigned, little-endian):
    uint32_t sampleRate = SDL_SwapLE32(config->sampleRate);
    ptr = memcpy(ptr, &sampleRate, 4) + 4;
    // 6. Output Gain (16 bits, signed, little-endian):
    int16_t outputGain = SDL_SwapLE16(0);
    ptr = memcpy(ptr, &outputGain, 2) + 2;
    // 7. Channel Mapping Family (8 bits, unsigned):
    *ptr++ = 1;
    // 8.1. Stream Count 'N' (8 bits, unsigned):
    *ptr++ = config->streams;
    // 8.2. Coupled Stream Count 'M' (8 bits, unsigned):
    *ptr++ = config->coupledStreams;
    // 8.3. Channel Mapping (8*C bits):
    ptr = memcpy(ptr, config->mapping, config->channelCount) + config->channelCount;
    return ptr - data;
}

AUDIO_RENDERER_CALLBACKS ss4s_aud_callbacks = {
        .init = aud_init,
        .cleanup = aud_cleanup,
        .decodeAndPlaySample = aud_feed,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

void session_audio_prepare_callbacks(void) {
#if TARGET_WEBOS
    /*
     * Opus passthrough (the default) hands each packet to NDL as is, so it runs on the RTP
     * receive thread like upstream moonlight-tv and punktfunk do. Without direct submit Limelight
     * queues audio in a 30-packet (150 ms) LBQ and, when the decoder thread falls behind (it
     * waits on the NDL lock the 4K video feed also holds), flushes the WHOLE queue: a 150 ms cut.
     * Direct submit was turned off in v1.2.3, the first version #108 reports cuts on.
     *
     * Only PCM mode decodes Opus here; keep that off the receive thread (#59, #66).
     */
    bool client_decode = app_configuration != NULL && app_configuration->surround_51_mode == SURROUND_51_PCM;
    ss4s_aud_callbacks.capabilities = client_decode ? 0 : CAPABILITY_DIRECT_SUBMIT;
    commons_log_info("Session", "Audio feed on %s", client_decode ? "Limelight decoder thread" : "receive thread");
#endif
}
