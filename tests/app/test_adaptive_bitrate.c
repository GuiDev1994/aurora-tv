#include "unity.h"
#include "stream/adaptive_bitrate.h"

#include <string.h>

static SERVER_DATA server;

void setUp(void) {
    memset(&server, 0, sizeof(server));
}

void tearDown(void) {
}

void test_runs_when_turned_on_for_other_hosts() {
    TEST_ASSERT_TRUE(adaptive_bitrate_client_runs(true, &server));
    server.isGfe = true;
    TEST_ASSERT_TRUE(adaptive_bitrate_client_runs(true, &server));
}

void test_stays_off_when_turned_off() {
    TEST_ASSERT_FALSE(adaptive_bitrate_client_runs(false, &server));
}

void test_leaves_the_bitrate_to_rubylight() {
    server.isRubylight = true;
    TEST_ASSERT_FALSE(adaptive_bitrate_client_runs(true, &server));
}

void test_no_server_no_client_bitrate() {
    TEST_ASSERT_FALSE(adaptive_bitrate_client_runs(true, NULL));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_runs_when_turned_on_for_other_hosts);
    RUN_TEST(test_stays_off_when_turned_off);
    RUN_TEST(test_leaves_the_bitrate_to_rubylight);
    RUN_TEST(test_no_server_no_client_bitrate);
    return UNITY_END();
}
