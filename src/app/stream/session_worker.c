#include "session_worker.h"
#include "session_priv.h"
#include "app.h"
#include "util/bus.h"
#include "logging.h"
#include "errors.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/connection/session_connection.h"
#include "stream/audio/session_audio.h"
#include "stream/video/session_video.h"
#include "stream/adaptive_bitrate.h"
#include "app_session.h"
#include "backend/pcmanager/worker/worker.h"
#include "app_settings.h"

#if TARGET_WEBOS
#include "platform/webos/game_mode.h"
#endif

#include <stdlib.h>
#include <stdio.h>

static void session_apply_decoder_env(const session_t *session) {
#if TARGET_WEBOS
    /* C5 native compositor is 120Hz. SDL_webOSGetRefreshRate often returns 144
     * (HDMI VRR max). Snapping PTS to 144Hz on a 120Hz plane is 5:6 pulldown —
     * pan hitch at every FPS. Wall-clock PTS tested best on-device for PAN. */
    setenv("SS4S_SMOOTH_PACING", "0", 1);
    setenv("SS4S_NDL_SMOOTH_PACING", "0", 1);
    setenv("SS4S_PANEL_PHASE_PACING", "0", 1);
    unsetenv("SS4S_SMOOTH_PACING_HOST_ONLY");
    unsetenv("SS4S_PRESENTATION_OFFSET_US");
    unsetenv("SS4S_SMOOTH_PACING_INTERVAL_US");
    unsetenv("SS4S_NDL_PACING_INTERVAL_US");
    unsetenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES");
    unsetenv("SS4S_PANEL_PHASE_INTERVAL_US");
    setenv("SS4S_PAUSE_AT_DECODE_TIME", "1", 1);
    commons_log_info("Session", "Stream pacing: wall-clock PTS (native 120Hz, no 144 VRR grid)");
    (void) session;
#else
    (void) session;
#endif
}

int session_worker(session_t *session) {
    app_t *app = session->app;
    session_set_state(session, STREAMING_CONNECTING);
    bus_pushevent(USER_STREAM_CONNECTING, NULL, NULL);
    streaming_error(session, GS_OK, "");
    PSERVER_DATA server = session->server;
    int appId = session->app_id;
    session->player = NULL;
#if TARGET_WEBOS
    webos_game_mode_state_t *game_mode_state = NULL;
#endif

#if FEATURE_INPUT_EVMOUSE
    if (!session->config.view_only && session->config.hardware_mouse) {
        session_evmouse_wait_ready(&session->input.evmouse);
    }
#endif

    commons_log_info("Session", "Launch app %d (host currentGame=%d)...", appId, server->currentGame);
    if (session->config.stream.clientRefreshRateX100 > 0) {
        commons_log_info("Session",
                         "Stream mode %dx%dx%d, clientRefreshRateX100=%d (%.2f Hz), bitrate %d kbps",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.clientRefreshRateX100,
                         session->config.stream.clientRefreshRateX100 / 100.0,
                         session->config.stream.bitrate);
    } else {
        commons_log_info("Session", "Stream mode %dx%dx%d, bitrate %d kbps (no fractional refresh rate)",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.bitrate);
    }
    GS_CLIENT client = app_gs_client_new(app);
    /* Host keeps SDL/Vorbis channel order. webOS SMP/NDL remaps PCM to device
     * order in Feed (SS4S_WebOS_RemapPcm51ToDevice). Do not send surroundParams. */
    const char *surround_params = NULL;
    short gamepad_mask = app_input_gamepads_mask(&app->input);
    int ret = gs_start_app(client, server, &session->config.stream, appId, server->isGfe, session->config.sops,
                           session->config.local_audio, gamepad_mask, surround_params);
    if (ret != GS_OK) {
        session_set_state(session, STREAMING_ERROR);
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        if (gs_error) {
            streaming_error(session, ret, "Failed to launch session: %s (code %d)", gs_error, ret);
        } else {
            streaming_error(session, ret, "Failed to launch session: gamestream returned %d", ret);
        }
        commons_log_error("Session", "Failed to launch session: gamestream returned %d, gs_error=%s", ret, gs_error);
        goto thread_cleanup;
    }

    commons_log_info("Session", "Audio %d channels",
                     CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(session->config.stream.audioConfiguration));

    session->player = SS4S_PlayerOpen();
    SS4S_PlayerSetWaitAudioVideoReady(session->player, true);
    SS4S_PlayerSetViewportSize(session->player, app->ui.width, app->ui.height);
    SS4S_PlayerSetUserdata(session->player, app);

    session_apply_decoder_env(session);
    session_video_prepare_stream();

#if TARGET_WEBOS
    if (session->app->settings.game_mode) {
        const bool hdr = session->app->settings.hdr &&
                         (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) != 0;
        game_mode_state = webos_game_mode_enter(hdr);
    }
#endif

    int startResult = LiStartConnection(&server->serverInfo, &session->config.stream,
                                        session_connection_callbacks_prepare(session),
                                        &ss4s_dec_callbacks, &ss4s_aud_callbacks, session, 0, session, 0);
    if (startResult != 0) {
        session_set_state(session, STREAMING_ERROR);
        switch (startResult) {
            case CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported video codec.");
                break;
            case CALLBACKS_SESSION_ERROR_VDEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open video decoder.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported audio codec.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open audio backend.");
                break;
            default: {
                if (!streaming_errno) {
                    streaming_error(session, GS_WRONG_STATE, "Failed to start connection: Limelight returned %d (%s)",
                                    startResult, strerror(startResult));
                }
                break;
            }
        }
        commons_log_error("Session", "Failed to start connection: Limelight returned %d", startResult);
        goto thread_cleanup;
    }
    session_set_state(session, STREAMING_STREAMING);
    bus_pushevent(USER_STREAM_OPEN, NULL, NULL);
    if (session->config.auto_adjust_bitrate) {
        adaptive_bitrate_config_t abr_config = {
            .gs_client = client,
            .server = server,
            .initial_bitrate = session->config.stream.bitrate,
            .mode = (abr_mode_t) session->config.abr_mode,
            .recovery_only = false,
        };
        session->abr = adaptive_bitrate_start(&abr_config);
    }
    SDL_LockMutex(session->mutex);
    while (!session->interrupted) {
        // Wait until interrupted
        SDL_CondWait(session->cond, session->mutex);
    }
    SDL_UnlockMutex(session->mutex);
    bus_pushevent(USER_STREAM_CLOSE, NULL, NULL);

    session_set_state(session, STREAMING_DISCONNECTING);
    LiStopConnection();

    if (session->quitapp) {
        commons_log_info("Session", "Sending app quit request ...");
        gs_quit_app(client, server);
    }
    worker_context_t update_ctx = {
            .app = app,
            .manager = pcmanager,
    };
    uuidstr_fromstr(&update_ctx.uuid, server->uuid);
    pcmanager_update_by_host(&update_ctx, server->serverInfo.address, server->extPort, true);

    // Don't always reset status as error state should be kept
    session_set_state(session, STREAMING_NONE);
    thread_cleanup:
#if TARGET_WEBOS
    webos_game_mode_restore(game_mode_state);
    game_mode_state = NULL;
#endif
    /* Restore only on a clean exit: streaming_errno != GS_OK means the session
     * ended in error/disconnect and the host is likely unreachable -- the
     * restore round-trips would just block teardown on timeouts. */
    adaptive_bitrate_stop(session->abr, streaming_errno == GS_OK);
    session->abr = NULL;
    session_connection_callbacks_reset(session);
    if (session->player != NULL) {
        SS4S_PlayerClose(session->player);
    }
    gs_destroy(client);
    bus_pushevent(USER_STREAM_FINISHED, NULL, NULL);
    app_bus_post(app, (bus_actionfunc) app_session_destroy, app);
    return 0;
}