#include "microphone_capture.h"

#include <Limelight.h>
#include <opus.h>
#include <SDL.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

#define MIC_SAMPLE_RATE 48000
#define MIC_CHANNELS 1
#define MIC_FRAME_SIZE 960
#define MIC_BITRATE 64000
#define MIC_PACKET_BUFFER_SIZE 1400
#define MIC_MAX_BUFFERED_SAMPLES (MIC_FRAME_SIZE * 12)
#define MIC_FRAME_DURATION_US ((uint64_t) MIC_FRAME_SIZE * 1000000ULL / MIC_SAMPLE_RATE)

struct microphone_capture_t {
    SDL_AudioDeviceID device_id;
    SDL_AudioSpec obtained_spec;
    OpusEncoder *encoder;
    opus_int16 *sample_buffer;
    size_t sample_buffer_start;
    size_t sample_buffer_count;
    SDL_mutex *buffer_mutex;
    SDL_cond *buffer_cond;
    SDL_Thread *encoder_thread;
    bool streaming;
    bool stop_thread;
    bool first_packet_logged;
    unsigned char encoded_packet[MIC_PACKET_BUFFER_SIZE];
};

static void microphone_capture_audio_cb(void *userdata, Uint8 *stream, int len);

static int microphone_capture_encoder_thread(void *data);

static void microphone_capture_clear_locked(microphone_capture_t *capture);

static void microphone_capture_push_locked(microphone_capture_t *capture, const opus_int16 *samples, size_t count);

static bool microphone_capture_pop_locked(microphone_capture_t *capture, opus_int16 *samples, size_t count);

static uint64_t microphone_capture_now_us(uint64_t performance_frequency);

static void microphone_capture_delay_until_us(uint64_t target_us, uint64_t performance_frequency);

static void microphone_capture_audio_cb(void *userdata, Uint8 *stream, int len) {
    microphone_capture_t *capture = userdata;
    if (capture == NULL || stream == NULL || len <= 0) {
        return;
    }

    SDL_LockMutex(capture->buffer_mutex);
    if (capture->streaming) {
        microphone_capture_push_locked(capture, (const opus_int16 *) stream, (size_t) len / sizeof(opus_int16));
        SDL_CondSignal(capture->buffer_cond);
    }
    SDL_UnlockMutex(capture->buffer_mutex);
}

microphone_capture_t *microphone_capture_create(const char *device_name) {
    microphone_capture_t *capture = calloc(1, sizeof(microphone_capture_t));
    if (capture == NULL) {
        return NULL;
    }

    capture->buffer_mutex = SDL_CreateMutex();
    capture->buffer_cond = SDL_CreateCond();
    capture->sample_buffer = calloc(MIC_MAX_BUFFERED_SAMPLES, sizeof(opus_int16));
    if (capture->buffer_mutex == NULL || capture->buffer_cond == NULL || capture->sample_buffer == NULL) {
        microphone_capture_destroy(capture);
        return NULL;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        commons_log_warn("Microphone", "Failed to initialize SDL audio for microphone capture: %s", SDL_GetError());
        microphone_capture_destroy(capture);
        return NULL;
    }

    int opus_error = OPUS_OK;
    capture->encoder = opus_encoder_create(MIC_SAMPLE_RATE, MIC_CHANNELS, OPUS_APPLICATION_VOIP, &opus_error);
    if (capture->encoder == NULL || opus_error != OPUS_OK) {
        commons_log_warn("Microphone", "Failed to create Opus encoder for microphone capture: %s",
                         opus_strerror(opus_error));
        microphone_capture_destroy(capture);
        return NULL;
    }

    opus_encoder_ctl(capture->encoder, OPUS_SET_BITRATE(MIC_BITRATE));
    opus_encoder_ctl(capture->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(capture->encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(capture->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(capture->encoder, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(capture->encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(capture->encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(capture->encoder, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(capture->encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_20_MS));

    SDL_AudioSpec desired = {0};
    desired.freq = MIC_SAMPLE_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = MIC_CHANNELS;
    desired.samples = MIC_FRAME_SIZE;
    desired.callback = microphone_capture_audio_cb;
    desired.userdata = capture;

    const char *selected_device = device_name != NULL && device_name[0] != '\0' ? device_name : NULL;
    capture->device_id = SDL_OpenAudioDevice(selected_device, SDL_TRUE, &desired, &capture->obtained_spec, 0);
    if (capture->device_id == 0 && selected_device != NULL) {
        commons_log_warn("Microphone", "Failed to open microphone '%s': %s. Falling back to default input.",
                         selected_device, SDL_GetError());
        capture->device_id = SDL_OpenAudioDevice(NULL, SDL_TRUE, &desired, &capture->obtained_spec, 0);
    }

    if (capture->device_id == 0) {
        commons_log_warn("Microphone", "Failed to open microphone capture device: %s", SDL_GetError());
        microphone_capture_destroy(capture);
        return NULL;
    }

    commons_log_info("Microphone", "Capture device %s (%d Hz, %d channels, format 0x%x, callback %d samples)",
                     selected_device != NULL ? selected_device : "<default>",
                     capture->obtained_spec.freq, capture->obtained_spec.channels,
                     capture->obtained_spec.format, capture->obtained_spec.samples);

    if (capture->obtained_spec.freq != MIC_SAMPLE_RATE ||
        capture->obtained_spec.channels != MIC_CHANNELS ||
        capture->obtained_spec.format != AUDIO_S16SYS) {
        commons_log_warn("Microphone", "Unsupported microphone capture format: %d Hz, %d channels, format 0x%x",
                         capture->obtained_spec.freq, capture->obtained_spec.channels, capture->obtained_spec.format);
        microphone_capture_destroy(capture);
        return NULL;
    }

    if (capture->obtained_spec.samples != MIC_FRAME_SIZE) {
        commons_log_info("Microphone",
                         "Backend callback size is %d samples; outgoing microphone packets stay paced at %d samples",
                         capture->obtained_spec.samples, MIC_FRAME_SIZE);
    }

    capture->encoder_thread = SDL_CreateThread(microphone_capture_encoder_thread, "mic-encoder", capture);
    if (capture->encoder_thread == NULL) {
        commons_log_warn("Microphone", "Failed to create microphone encoder thread: %s", SDL_GetError());
        microphone_capture_destroy(capture);
        return NULL;
    }

    SDL_PauseAudioDevice(capture->device_id, 1);
    return capture;
}

bool microphone_capture_start(microphone_capture_t *capture) {
    if (capture == NULL || capture->device_id == 0 || !LiIsMicrophoneStreamActive()) {
        return false;
    }

    SDL_LockMutex(capture->buffer_mutex);
    microphone_capture_clear_locked(capture);
    capture->first_packet_logged = false;
    capture->streaming = true;
    SDL_CondSignal(capture->buffer_cond);
    SDL_UnlockMutex(capture->buffer_mutex);

    commons_log_info("Microphone", "Client microphone capture started");
    SDL_PauseAudioDevice(capture->device_id, 0);
    return true;
}

void microphone_capture_stop(microphone_capture_t *capture) {
    if (capture == NULL || capture->buffer_mutex == NULL) {
        return;
    }

    if (capture->device_id != 0) {
        SDL_PauseAudioDevice(capture->device_id, 1);
    }

    SDL_LockMutex(capture->buffer_mutex);
    capture->streaming = false;
    microphone_capture_clear_locked(capture);
    SDL_CondSignal(capture->buffer_cond);
    SDL_UnlockMutex(capture->buffer_mutex);
}

void microphone_capture_destroy(microphone_capture_t *capture) {
    if (capture == NULL) {
        return;
    }

    microphone_capture_stop(capture);

    if (capture->buffer_mutex != NULL) {
        SDL_LockMutex(capture->buffer_mutex);
        capture->stop_thread = true;
        if (capture->buffer_cond != NULL) {
            SDL_CondSignal(capture->buffer_cond);
        }
        SDL_UnlockMutex(capture->buffer_mutex);
    }

    if (capture->encoder_thread != NULL) {
        SDL_WaitThread(capture->encoder_thread, NULL);
    }

    if (capture->device_id != 0) {
        SDL_CloseAudioDevice(capture->device_id);
    }
    if (capture->encoder != NULL) {
        opus_encoder_destroy(capture->encoder);
    }
    if (capture->buffer_cond != NULL) {
        SDL_DestroyCond(capture->buffer_cond);
    }
    if (capture->buffer_mutex != NULL) {
        SDL_DestroyMutex(capture->buffer_mutex);
    }

    free(capture->sample_buffer);
    free(capture);
}

static int microphone_capture_encoder_thread(void *data) {
    microphone_capture_t *capture = data;
    const uint64_t performance_frequency = SDL_GetPerformanceFrequency();
    opus_int16 frame[MIC_FRAME_SIZE];
    uint64_t next_send_deadline_us = 0;
    bool pacing_active = false;

    for (;;) {
        SDL_LockMutex(capture->buffer_mutex);
        while (!capture->stop_thread && (!capture->streaming || capture->sample_buffer_count < MIC_FRAME_SIZE)) {
            if (!capture->streaming) {
                pacing_active = false;
            }
            SDL_CondWait(capture->buffer_cond, capture->buffer_mutex);
        }
        if (capture->stop_thread) {
            SDL_UnlockMutex(capture->buffer_mutex);
            break;
        }
        if (!microphone_capture_pop_locked(capture, frame, MIC_FRAME_SIZE)) {
            pacing_active = false;
            SDL_UnlockMutex(capture->buffer_mutex);
            continue;
        }
        SDL_UnlockMutex(capture->buffer_mutex);

        uint64_t now_us = microphone_capture_now_us(performance_frequency);
        if (!pacing_active) {
            next_send_deadline_us = now_us;
            pacing_active = true;
        } else if (now_us > next_send_deadline_us + MIC_FRAME_DURATION_US * 2) {
            next_send_deadline_us = now_us;
        }

        if (next_send_deadline_us > now_us) {
            microphone_capture_delay_until_us(next_send_deadline_us, performance_frequency);
        }
        next_send_deadline_us += MIC_FRAME_DURATION_US;

        int encoded_bytes = opus_encode(capture->encoder, frame, MIC_FRAME_SIZE, capture->encoded_packet,
                                        (opus_int32) sizeof(capture->encoded_packet));
        if (encoded_bytes <= 0) {
            continue;
        }

        int send_result = LiSendMicrophoneOpusDataEx(capture->encoded_packet, encoded_bytes, MIC_FRAME_SIZE);
        if (send_result >= 0 && !capture->first_packet_logged) {
            commons_log_info("Microphone", "Sent first microphone packet (%d bytes Opus)", encoded_bytes);
            capture->first_packet_logged = true;
        } else if (send_result < 0) {
            commons_log_warn("Microphone", "Failed to send microphone packet to host");
        }
    }

    return 0;
}

static void microphone_capture_clear_locked(microphone_capture_t *capture) {
    capture->sample_buffer_start = 0;
    capture->sample_buffer_count = 0;
}

static void microphone_capture_push_locked(microphone_capture_t *capture, const opus_int16 *samples, size_t count) {
    if (count >= MIC_MAX_BUFFERED_SAMPLES) {
        samples += count - MIC_MAX_BUFFERED_SAMPLES;
        count = MIC_MAX_BUFFERED_SAMPLES;
        microphone_capture_clear_locked(capture);
    }

    if (capture->sample_buffer_count + count > MIC_MAX_BUFFERED_SAMPLES) {
        size_t drop = capture->sample_buffer_count + count - MIC_MAX_BUFFERED_SAMPLES;
        capture->sample_buffer_start = (capture->sample_buffer_start + drop) % MIC_MAX_BUFFERED_SAMPLES;
        capture->sample_buffer_count -= drop;
    }

    size_t write_pos = (capture->sample_buffer_start + capture->sample_buffer_count) % MIC_MAX_BUFFERED_SAMPLES;
    size_t first_chunk = MIC_MAX_BUFFERED_SAMPLES - write_pos;
    if (first_chunk > count) {
        first_chunk = count;
    }

    memcpy(capture->sample_buffer + write_pos, samples, first_chunk * sizeof(opus_int16));
    if (count > first_chunk) {
        memcpy(capture->sample_buffer, samples + first_chunk, (count - first_chunk) * sizeof(opus_int16));
    }

    capture->sample_buffer_count += count;
}

static bool microphone_capture_pop_locked(microphone_capture_t *capture, opus_int16 *samples, size_t count) {
    if (capture->sample_buffer_count < count) {
        return false;
    }

    size_t first_chunk = MIC_MAX_BUFFERED_SAMPLES - capture->sample_buffer_start;
    if (first_chunk > count) {
        first_chunk = count;
    }

    memcpy(samples, capture->sample_buffer + capture->sample_buffer_start, first_chunk * sizeof(opus_int16));
    if (count > first_chunk) {
        memcpy(samples + first_chunk, capture->sample_buffer, (count - first_chunk) * sizeof(opus_int16));
    }

    capture->sample_buffer_start = (capture->sample_buffer_start + count) % MIC_MAX_BUFFERED_SAMPLES;
    capture->sample_buffer_count -= count;
    return true;
}

static uint64_t microphone_capture_now_us(uint64_t performance_frequency) {
    return SDL_GetPerformanceCounter() * 1000000ULL / performance_frequency;
}

static void microphone_capture_delay_until_us(uint64_t target_us, uint64_t performance_frequency) {
    for (;;) {
        uint64_t now_us = microphone_capture_now_us(performance_frequency);
        if (now_us >= target_us) {
            return;
        }

        uint64_t remaining_us = target_us - now_us;
        if (remaining_us > 1000) {
            SDL_Delay((Uint32) (remaining_us / 1000));
        } else {
            SDL_Delay(1);
        }
    }
}
