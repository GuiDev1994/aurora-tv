#include "unity.h"
#include "stream/host_failure.h"

#include <Limelight.h>
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

void test_host_ended_codes() {
    TEST_ASSERT_TRUE(host_failure_ended_stream(ML_ERROR_UNEXPECTED_EARLY_TERMINATION));
    TEST_ASSERT_TRUE(host_failure_ended_stream(ML_ERROR_PROTECTED_CONTENT));
    TEST_ASSERT_TRUE(host_failure_ended_stream(ML_ERROR_FRAME_CONVERSION));
    /* E_FAIL, as the host passes an encoder failure through. */
    TEST_ASSERT_TRUE(host_failure_ended_stream((int) 0x80004005u));
}

void test_other_codes_are_not_the_host() {
    TEST_ASSERT_FALSE(host_failure_ended_stream(ML_ERROR_GRACEFUL_TERMINATION));
    TEST_ASSERT_FALSE(host_failure_ended_stream(ML_ERROR_NO_VIDEO_TRAFFIC));
    TEST_ASSERT_FALSE(host_failure_ended_stream(ML_ERROR_NO_VIDEO_FRAME));
    /* A socket error such as ECONNRESET. */
    TEST_ASSERT_FALSE(host_failure_ended_stream(104));
    TEST_ASSERT_FALSE(host_failure_ended_stream(-1));
}

void test_appends_after_a_blank_line() {
    char message[64] = "Connection terminated";
    host_failure_append(message, sizeof(message), "Reason from the PC: HDR is off");
    TEST_ASSERT_EQUAL_STRING("Connection terminated\n\nReason from the PC: HDR is off", message);
}

void test_an_empty_message_takes_the_line() {
    char message[32] = "";
    host_failure_append(message, sizeof(message), "Reason");
    TEST_ASSERT_EQUAL_STRING("Reason", message);
}

void test_stays_within_the_buffer() {
    char message[16] = "Terminated";
    host_failure_append(message, sizeof(message), "a reason that does not fit");
    TEST_ASSERT_EQUAL_INT(15, (int) strlen(message));
    TEST_ASSERT_EQUAL_STRING("Terminated\n\na r", message);
}

void test_nothing_to_add() {
    char message[16] = "Terminated";
    host_failure_append(message, sizeof(message), "");
    host_failure_append(message, sizeof(message), NULL);
    TEST_ASSERT_EQUAL_STRING("Terminated", message);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_host_ended_codes);
    RUN_TEST(test_other_codes_are_not_the_host);
    RUN_TEST(test_appends_after_a_blank_line);
    RUN_TEST(test_an_empty_message_takes_the_line);
    RUN_TEST(test_stays_within_the_buffer);
    RUN_TEST(test_nothing_to_add);
    return UNITY_END();
}
