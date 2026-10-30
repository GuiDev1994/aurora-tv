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

#include "http.h"
#include "errors.h"
#include "set_error.h"
#include "logging.h"

#include <string.h>
#include <strings.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

#ifdef __WIN32
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif

struct HTTP_T {
    CURL *curl;
    pthread_mutex_t mutex;
};

static size_t write_fn(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    HTTP_DATA *mem = (HTTP_DATA *) userp;

    void *allocated = realloc(mem->memory, mem->size + realsize + 1);
    assert(allocated != NULL);
    mem->memory = allocated;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

HTTP *http_create(const char *keydir) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        gs_set_error(GS_ERROR, "Failed to create cURL instance");
        return NULL;
    }

    char certificateFilePath[4096];
    sprintf(certificateFilePath, "%s%c%s", keydir, PATH_SEPARATOR, CERTIFICATE_FILE_NAME);

    char keyFilePath[4096];
    sprintf(&keyFilePath[0], "%s%c%s", keydir, PATH_SEPARATOR, KEY_FILE_NAME);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLENGINE_DEFAULT, 1L);
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLCERT, certificateFilePath);
    curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEY, keyFilePath);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_fn);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    // Fix for https://github.com/mariotaku/moonlight-tv/issues/452
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

    struct HTTP_T *http = malloc(sizeof(struct HTTP_T));
    assert(http != NULL);
    http->curl = curl;
    pthread_mutex_init(&http->mutex, NULL);
    return http;
}

int http_request(HTTP *http, char *url, HTTP_DATA *data) {
    assert(http != NULL);
    assert(data != NULL);
    if (data->size > 0) {
        void *allocated = realloc(data->memory, 1);
        assert(allocated != NULL);
        data->memory = allocated;
        data->memory[0] = 0;
        data->size = 0;
    }
    pthread_mutex_lock(&http->mutex);
    CURL *curl = http->curl;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    commons_log_debug("GameStream", "Request %p %s", data, url);

    int ret = GS_FAILED;
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_HTTP_RETURNED_ERROR) {
        int http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        commons_log_warn("GameStream", "Request %p error HTTP %d", data, http_status);
        goto finish;
    } else if (res != CURLE_OK) {
        const char *errmsg = curl_easy_strerror(res);
        ret = gs_set_error(GS_IO_ERROR, "cURL error: %s", errmsg);
        commons_log_debug("GameStream", "Request %p error %d: %s", data, ret, errmsg);
        goto finish;
    }
    assert (data->memory != NULL);
    int http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_code);
    commons_log_debug("GameStream", "Request %p response %d", data, http_code);
    commons_log_hexdump(COMMONS_LOG_LEVEL_VERBOSE, "GameStream", data->memory, data->size);

    ret = GS_OK;
    finish:
    pthread_mutex_unlock(&http->mutex);
    return ret;
}

int http_post_json(HTTP *http, char *url, const char *payload, HTTP_DATA *data) {
    assert(http != NULL);
    assert(data != NULL);
    if (data->size > 0) {
        void *allocated = realloc(data->memory, 1);
        assert(allocated != NULL);
        data->memory = allocated;
        data->memory[0] = 0;
        data->size = 0;
    }
    pthread_mutex_lock(&http->mutex);
    CURL *curl = http->curl;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(payload));
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    int ret = GS_FAILED;
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);

    if (res == CURLE_HTTP_RETURNED_ERROR) {
        goto finish;
    } else if (res != CURLE_OK) {
        ret = gs_set_error(GS_IO_ERROR, "cURL error: %s", curl_easy_strerror(res));
        goto finish;
    }
    ret = GS_OK;
    finish:
    pthread_mutex_unlock(&http->mutex);
    return ret;
}

typedef struct {
    HTTP_PROBE *probe;
    size_t max_bytes;
    bool too_long;
    bool headers_done;
    struct timespec body_start;
} probe_state_t;

static size_t probe_header_fn(char *buffer, size_t size, size_t nitems, void *userp) {
    probe_state_t *state = userp;
    size_t len = size * nitems;
    if (len > 0 && len <= 2 && (buffer[0] == '\r' || buffer[0] == '\n')) {
        clock_gettime(CLOCK_MONOTONIC, &state->body_start);
        state->headers_done = true;
    } else if (len > 12 && strncasecmp(buffer, "Retry-After:", 12) == 0) {
        state->probe->retry_after = (int) strtol(buffer + 12, NULL, 10);
    }
    return len;
}

static size_t probe_write_fn(void *contents, size_t size, size_t nmemb, void *userp) {
    (void) contents;
    probe_state_t *state = userp;
    size_t len = size * nmemb;
    if (state->probe->bytes + len > state->max_bytes) {
        state->too_long = true;
        return 0;
    }
    state->probe->bytes += len;
    return len;
}

int http_probe_download(HTTP *http, const char *url, size_t max_bytes, int timeout_secs, HTTP_PROBE *probe) {
    assert(http != NULL);
    assert(probe != NULL);
    memset(probe, 0, sizeof(*probe));
    // A handle of its own, so other requests on this client don't wait seconds for the body.
    pthread_mutex_lock(&http->mutex);
    CURL *curl = curl_easy_duphandle(http->curl);
    pthread_mutex_unlock(&http->mutex);
    if (curl == NULL) {
        return gs_set_error(GS_OUT_OF_MEMORY, "Failed to create cURL instance");
    }
    probe_state_t state = {.probe = probe, .max_bytes = max_bytes};
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) timeout_secs);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, probe_header_fn);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_write_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

    commons_log_debug("GameStream", "Probe %s", url);
    CURLcode res = curl_easy_perform(curl);
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &probe->http_status);
    char *content_type = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    bool octet_stream = content_type != NULL && strncasecmp(content_type, "application/octet-stream", 24) == 0;

    int ret;
    if (state.too_long) {
        ret = gs_set_error(GS_INVALID, "Probe body is longer than %zu bytes", max_bytes);
    } else if (res != CURLE_OK) {
        ret = gs_set_error(GS_IO_ERROR, "cURL error: %s", curl_easy_strerror(res));
    } else if (probe->http_status != 200) {
        ret = gs_set_error(GS_FAILED, "Probe refused with HTTP %ld", probe->http_status);
    } else if (!octet_stream || probe->bytes == 0 || !state.headers_done) {
        ret = gs_set_error(GS_INVALID, "Probe answered no application/octet-stream body");
    } else {
        double seconds = (double) (end.tv_sec - state.body_start.tv_sec) +
                         (double) (end.tv_nsec - state.body_start.tv_nsec) / 1e9;
        probe->seconds = seconds > 1e-6 ? seconds : 1e-6;
        ret = GS_OK;
    }
    curl_easy_cleanup(curl);
    return ret;
}

void http_set_user_agent(HTTP *http, const char *user_agent) {
    assert(http != NULL);
    pthread_mutex_lock(&http->mutex);
    curl_easy_setopt(http->curl, CURLOPT_USERAGENT, user_agent);
    pthread_mutex_unlock(&http->mutex);
}

void http_destroy(HTTP *http) {
    assert(http != NULL);
    pthread_mutex_lock(&http->mutex);
    curl_easy_cleanup(http->curl);
    pthread_mutex_unlock(&http->mutex);
    pthread_mutex_destroy(&http->mutex);
    free((void *) http);
}

void http_set_timeout(HTTP *http, int timeout) {
    assert(http != NULL);
    pthread_mutex_lock(&http->mutex);
    curl_easy_setopt(http->curl, CURLOPT_CONNECTTIMEOUT, timeout);
    pthread_mutex_unlock(&http->mutex);
}

/* Whole-transfer ceiling (CURLOPT_TIMEOUT), not just connect: without one, a
 * host that accepts the TCP connection and then hangs stalls the caller
 * forever. 0 restores curl's default (no total timeout). */
void http_set_total_timeout(HTTP *http, int timeout) {
    assert(http != NULL);
    pthread_mutex_lock(&http->mutex);
    curl_easy_setopt(http->curl, CURLOPT_TIMEOUT, (long) timeout);
    pthread_mutex_unlock(&http->mutex);
}

HTTP_DATA *http_data_alloc() {
    HTTP_DATA *data = malloc(sizeof(HTTP_DATA));
    assert(data != NULL);

    data->memory = malloc(1);
    assert(data->memory != NULL);
    data->size = 0;

    return data;
}

void http_data_free(HTTP_DATA *data) {
    if (data == NULL) {
        return;
    }
    if (data->memory != NULL) {
        free(data->memory);
    }

    free(data);
}
