/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "xml.h"

#include <Limelight.h>

#include <stdbool.h>
#include <stddef.h>

#define MIN_SUPPORTED_GFE_VERSION 3
#define MAX_SUPPORTED_GFE_VERSION 7

#define WAKE_METHOD_WOL 0
#define WAKE_METHOD_HTTP 1

typedef struct _SERVER_DATA {
    const char *uuid;
    const char *mac;
    const char *hostname;
    const char *gpuType;
    /** HttpsPort */
    unsigned short httpsPort;
    /** ExternalPort */
    unsigned short extPort;
    bool paired;
    bool supports4K;
    bool supportsHdr;
    bool unsupported;
    bool isGfe;
    int currentGame;
    int serverMajorVersion;
    const char *gsVersion;
    int wake_method;
    char *wake_url;
    /** BandwidthProbeVersion (Rubylight 3.0+): the host serves gs_probe_bandwidth. 0 when absent. */
    int bandwidthProbeVersion;
    PDISPLAY_MODE modes;
    SERVER_INFORMATION serverInfo;
} SERVER_DATA, *PSERVER_DATA;

typedef struct GS_CLIENT_T *GS_CLIENT;

GS_CLIENT gs_new(const char *keydir);

int gs_conf_init(const char *keydir);

void gs_destroy(GS_CLIENT hnd);

void gs_set_timeout(GS_CLIENT hnd, int timeout_secs);

/* Whole-transfer ceiling on every request made through this client (see
 * http_set_total_timeout); 0 = no ceiling (curl default). */
void gs_set_total_timeout(GS_CLIENT hnd, int timeout_secs);

/* User-Agent of every request made through this client. Rubylight lists it per device. */
void gs_set_user_agent(GS_CLIENT hnd, const char *user_agent);

int gs_get_status(GS_CLIENT hnd, PSERVER_DATA server, const char *address, uint16_t port, bool unsupported);

/* hdr: the user asked for HDR. A 10-bit format offered without it is 10-bit SDR. */
int gs_start_app(GS_CLIENT hnd, PSERVER_DATA server, PSTREAM_CONFIGURATION config, int appId, bool is_gfe, bool sops,
                 bool localaudio, int gamepad_mask, const char *surround_params, bool hdr);

/* Appends hdrMode and the client HDR capabilities to a launch/resume URL when a 10-bit format
 * is offered. hdrMode=0 for 10-bit SDR: Sunshine-family hosts read hdrMode=1 as "switch the
 * display to HDR", whatever the format. */
void gs_append_hdr_launch_params(char *url, size_t url_len, int supported_video_formats, bool hdr);

int gs_applist(GS_CLIENT hnd, const SERVER_DATA *server, PAPP_LIST *app_list);

int gs_unpair(GS_CLIENT hnd, PSERVER_DATA server);

int gs_pair(GS_CLIENT hnd, PSERVER_DATA server, const char *pin);

int gs_quit_app(GS_CLIENT hnd, PSERVER_DATA server);

int gs_download_cover(GS_CLIENT hnd, const SERVER_DATA *server, int appId, const char *path);

typedef struct {
    bool supported;
    int version;
} GS_ABR_CAPABILITIES;

typedef struct {
    bool enabled;
    int min_bitrate;
    int max_bitrate;
    const char *mode;
} GS_ABR_CONFIG;

typedef struct {
    float packet_loss;
    int rtt_ms;
    float decode_fps;
    int dropped_frames;
    int current_bitrate;
} GS_ABR_FEEDBACK;

typedef struct {
    bool has_new_bitrate;
    int new_bitrate;
} GS_ABR_ACTION;

int gs_set_bitrate(GS_CLIENT hnd, const SERVER_DATA *server, int bitrate_kbps);

int gs_get_abr_capabilities(GS_CLIENT hnd, const SERVER_DATA *server, GS_ABR_CAPABILITIES *caps);

int gs_set_abr_mode(GS_CLIENT hnd, const SERVER_DATA *server, const GS_ABR_CONFIG *config);

int gs_report_abr_feedback(GS_CLIENT hnd, const SERVER_DATA *server, const GS_ABR_FEEDBACK *feedback,
                           GS_ABR_ACTION *action);

int gs_http_wake(GS_CLIENT hnd, const char *url);

/* The largest body GET /bandwidth-probe serves. */
#define GS_BANDWIDTH_PROBE_BYTES (4 * 1024 * 1024)

typedef struct {
    double mbps;
    /** HTTP status of a refusal (409 while a stream runs or starts, 429 too soon), else 0. */
    int refused_status;
    /** Seconds a 429 asks to wait; 0 when absent. */
    int retry_after_secs;
} GS_BANDWIDTH_PROBE;

/* Times GS_BANDWIDTH_PROBE_BYTES of GET /bandwidth-probe from a host with bandwidthProbeVersion,
 * from its headers to the end of its body. The host stops after 3 s, so a short body on a slow
 * link is still a measurement. A refusal is GS_FAILED with refused_status set. */
int gs_probe_bandwidth(GS_CLIENT hnd, const SERVER_DATA *server, GS_BANDWIDTH_PROBE *probe);