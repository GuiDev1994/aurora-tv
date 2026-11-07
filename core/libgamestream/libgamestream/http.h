/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
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

#include <stddef.h>

#define CERTIFICATE_FILE_NAME "client.pem"
#define KEY_FILE_NAME "key.pem"

typedef struct HTTP_T HTTP;

typedef struct _HTTP_DATA {
    char *memory;
    size_t size;
} HTTP_DATA;

typedef struct {
    /** Status of the answer; 0 when none arrived. */
    long http_status;
    /** Body bytes, and seconds from the end of the headers to the end of the body. */
    size_t bytes;
    double seconds;
    /** Retry-After of the answer in seconds; 0 when absent. */
    int retry_after;
} HTTP_PROBE;

HTTP *http_create(const char *keydir);

int http_request(HTTP *http, char *url, HTTP_DATA * data);

/**
 * GETs url and counts its body without keeping it, for at most timeout_secs. A 200 answer that is
 * not application/octet-stream, or longer than max_bytes, fails with GS_INVALID; any other status
 * is GS_FAILED with probe->http_status set.
 */
int http_probe_download(HTTP *http, const char *url, size_t max_bytes, int timeout_secs, HTTP_PROBE *probe);

void http_set_user_agent(HTTP *http, const char *user_agent);

int http_post_json(HTTP *http, char *url, const char *payload, HTTP_DATA *data);

void http_destroy(HTTP *http);

void http_set_timeout(HTTP *http, int timeout);

void http_set_total_timeout(HTTP *http, int timeout);

HTTP_DATA * http_data_alloc();

void http_data_free(HTTP_DATA * data);
