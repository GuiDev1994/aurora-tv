#include "unity.h"
#include "libgamestream/errors.h"
#include "libgamestream/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* One HTTP answer to one connection on 127.0.0.1, standing in for Rubylight's GET /bandwidth-probe. */
typedef struct {
    int listen_fd;
    char url[128];
    char head[512];
    size_t body_bytes;
    char request[4096];
    pthread_t thread;
    bool started;
} server_t;

static server_t server;
static HTTP *http;

static void *serve_once(void *arg) {
    server_t *s = arg;
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0) {
        return NULL;
    }
    size_t len = 0;
    while (len < sizeof(s->request) - 1) {
        ssize_t n = recv(fd, s->request + len, sizeof(s->request) - 1 - len, 0);
        if (n <= 0) {
            break;
        }
        len += (size_t) n;
        s->request[len] = '\0';
        if (strstr(s->request, "\r\n\r\n") != NULL) {
            break;
        }
    }
    send(fd, s->head, strlen(s->head), MSG_NOSIGNAL);
    char chunk[16384];
    memset(chunk, 0xA5, sizeof(chunk));
    size_t left = s->body_bytes;
    while (left > 0) {
        ssize_t n = send(fd, chunk, left < sizeof(chunk) ? left : sizeof(chunk), MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        left -= (size_t) n;
    }
    close(fd);
    return NULL;
}

static void serve(int status, const char *content_type, const char *extra_headers, size_t body_bytes) {
    snprintf(server.head, sizeof(server.head),
             "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
             status, content_type, body_bytes, extra_headers);
    server.body_bytes = body_bytes;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&server.thread, NULL, serve_once, &server));
    server.started = true;
}

void setUp(void) {
    memset(&server, 0, sizeof(server));
    server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(server.listen_fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    TEST_ASSERT_EQUAL_INT(0, bind(server.listen_fd, (struct sockaddr *) &addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(server.listen_fd, 1));
    socklen_t addr_len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(server.listen_fd, (struct sockaddr *) &addr, &addr_len));
    snprintf(server.url, sizeof(server.url), "http://127.0.0.1:%u/bandwidth-probe?bytes=4194304",
             ntohs(addr.sin_port));
    http = http_create("/nonexistent");
    TEST_ASSERT_NOT_NULL(http);
}

void tearDown(void) {
    if (server.started) {
        pthread_join(server.thread, NULL);
    }
    close(server.listen_fd);
    http_destroy(http);
}

void test_counts_an_octet_stream_body() {
    serve(200, "application/octet-stream", "Cache-Control: no-store\r\n", 1024 * 1024);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_OK, http_probe_download(http, server.url, 4 * 1024 * 1024, 10, &probe));
    TEST_ASSERT_EQUAL_INT(200, probe.http_status);
    TEST_ASSERT_EQUAL_size_t(1024 * 1024, probe.bytes);
    TEST_ASSERT_TRUE(probe.seconds > 0);
}

void test_a_busy_host_is_a_refusal() {
    serve(409, "text/plain", "", 39);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_FAILED, http_probe_download(http, server.url, 4 * 1024 * 1024, 10, &probe));
    TEST_ASSERT_EQUAL_INT(409, probe.http_status);
}

void test_a_rate_limit_reads_retry_after() {
    serve(429, "text/plain", "Retry-After: 7\r\n", 29);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_FAILED, http_probe_download(http, server.url, 4 * 1024 * 1024, 10, &probe));
    TEST_ASSERT_EQUAL_INT(429, probe.http_status);
    TEST_ASSERT_EQUAL_INT(7, probe.retry_after);
}

void test_another_content_type_is_no_measurement() {
    serve(200, "text/html", "", 4096);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_INVALID, http_probe_download(http, server.url, 4 * 1024 * 1024, 10, &probe));
}

void test_an_empty_body_is_no_measurement() {
    serve(200, "application/octet-stream", "", 0);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_INVALID, http_probe_download(http, server.url, 4 * 1024 * 1024, 10, &probe));
}

void test_a_body_past_the_limit_fails() {
    serve(200, "application/octet-stream", "", 256 * 1024);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_INVALID, http_probe_download(http, server.url, 64 * 1024, 10, &probe));
}

void test_sends_the_user_agent() {
    http_set_user_agent(http, "Aurora/1.3.3 (webOS)");
    serve(200, "application/octet-stream", "", 1024);
    HTTP_PROBE probe;
    TEST_ASSERT_EQUAL_INT(GS_OK, http_probe_download(http, server.url, 4 * 1024 * 1024, 10, &probe));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "\r\nUser-Agent: Aurora/1.3.3 (webOS)\r\n"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_counts_an_octet_stream_body);
    RUN_TEST(test_a_busy_host_is_a_refusal);
    RUN_TEST(test_a_rate_limit_reads_retry_after);
    RUN_TEST(test_another_content_type_is_no_measurement);
    RUN_TEST(test_an_empty_body_is_no_measurement);
    RUN_TEST(test_a_body_past_the_limit_fails);
    RUN_TEST(test_sends_the_user_agent);
    return UNITY_END();
}
