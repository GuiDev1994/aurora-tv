#include "unity.h"
#include "libgamestream/errors.h"
#include "libgamestream/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* One HTTP answer to one connection on 127.0.0.1, standing in for Rubylight's /api/native/v1. */
typedef struct {
    int listen_fd;
    unsigned short port;
    char answer[2048];
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
        const char *head_end = strstr(s->request, "\r\n\r\n");
        if (head_end == NULL) {
            continue;
        }
        const char *length = strstr(s->request, "Content-Length: ");
        size_t body = length != NULL && length < head_end ? strtoul(length + 16, NULL, 10) : 0;
        if (len >= (size_t) (head_end + 4 - s->request) + body) {
            break;
        }
    }
    send(fd, s->answer, strlen(s->answer), MSG_NOSIGNAL);
    close(fd);
    return NULL;
}

static void serve(int status, const char *body) {
    snprintf(server.answer, sizeof(server.answer),
             "HTTP/1.1 %d X\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s", status, strlen(body), body);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&server.thread, NULL, serve_once, &server));
    server.started = true;
}

static void url(char *out, size_t len, const char *path) {
    snprintf(out, len, "http://127.0.0.1:%u/api/native/v1/%s", server.port, path);
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
    server.port = ntohs(addr.sin_port);
    http = http_create("/nonexistent");
    TEST_ASSERT_NOT_NULL(http);
}

void tearDown(void) {
    if (server.started) {
        pthread_join(server.thread, NULL);
    }
    if (server.listen_fd >= 0) {
        close(server.listen_fd);
    }
    http_destroy(http);
}

void test_get_keeps_the_body() {
    serve(200, "{\"restart\":{\"available\":true,\"reason\":null}}");
    char target[256];
    url(target, sizeof(target), "power");
    HTTP_DATA *data = http_data_alloc();
    long status = -1;
    TEST_ASSERT_EQUAL_INT(GS_OK, http_request_keep_body(http, target, NULL, 10, data, &status));
    TEST_ASSERT_EQUAL_INT(200, status);
    TEST_ASSERT_EQUAL_STRING("{\"restart\":{\"available\":true,\"reason\":null}}", data->memory);
    TEST_ASSERT_EQUAL_INT(0, strncmp(server.request, "GET /api/native/v1/power HTTP/1.1\r\n", 35));
    http_data_free(data);
}

void test_an_error_status_keeps_the_body_that_explains_it() {
    const char *body = "{\"error\":{\"code\":\"host_in_use\",\"message\":\"Living room TV is streaming from this PC.\"}}";
    serve(409, body);
    char target[256];
    url(target, sizeof(target), "power/restart");
    HTTP_DATA *data = http_data_alloc();
    long status = 0;
    TEST_ASSERT_EQUAL_INT(GS_OK, http_request_keep_body(http, target, "{\"token\":\"ab\"}", 10, data, &status));
    TEST_ASSERT_EQUAL_INT(409, status);
    TEST_ASSERT_EQUAL_STRING(body, data->memory);
    http_data_free(data);
}

void test_post_sends_the_json_body() {
    http_set_user_agent(http, "Aurora/1.3.3 (webOS)");
    serve(202, "{\"action\":\"shutdown\"}");
    char target[256];
    url(target, sizeof(target), "power/shutdown");
    HTTP_DATA *data = http_data_alloc();
    long status = 0;
    TEST_ASSERT_EQUAL_INT(GS_OK, http_request_keep_body(http, target, "{\"token\":\"3f9c\"}", 10, data, &status));
    TEST_ASSERT_EQUAL_INT(202, status);
    TEST_ASSERT_EQUAL_INT(0, strncmp(server.request, "POST /api/native/v1/power/shutdown HTTP/1.1\r\n", 45));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "\r\nContent-Type: application/json\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "\r\nUser-Agent: Aurora/1.3.3 (webOS)\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(server.request, "\r\n\r\n{\"token\":\"3f9c\"}"));
    http_data_free(data);
}

void test_no_answer_is_an_io_error() {
    close(server.listen_fd);
    server.listen_fd = -1;
    char target[256];
    url(target, sizeof(target), "power");
    HTTP_DATA *data = http_data_alloc();
    long status = -1;
    TEST_ASSERT_EQUAL_INT(GS_IO_ERROR, http_request_keep_body(http, target, NULL, 5, data, &status));
    TEST_ASSERT_EQUAL_INT(0, status);
    http_data_free(data);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_get_keeps_the_body);
    RUN_TEST(test_an_error_status_keeps_the_body_that_explains_it);
    RUN_TEST(test_post_sends_the_json_body);
    RUN_TEST(test_no_answer_is_an_io_error);
    return UNITY_END();
}
