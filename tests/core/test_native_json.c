#include "unity.h"
#include "libgamestream/client.h"
#include "libgamestream/json.h"

#include <stdlib.h>
#include <string.h>

/* Rubylight's own fixtures (rust/core/native-api/tests/fixtures), plus an idle PC. */
static const char *const POWER_BUSY =
        "{\n"
        "  \"restart\": { \"available\": false, \"reason\": \"Living room TV is streaming from this PC.\" },\n"
        "  \"shutdown\": { \"available\": false, \"reason\": \"Living room TV is streaming from this PC.\" },\n"
        "  \"token\": null,\n"
        "  \"token_lifetime_s\": 30,\n"
        "  \"wake_from_off\": true,\n"
        "  \"last_refusal\": null,\n"
        "  \"last_refusal_unconfirmed\": false\n"
        "}\n";
static const char *const POWER_IDLE =
        "{\"restart\":{\"available\":true,\"reason\":null},\"shutdown\":{\"available\":true,\"reason\":null},"
        "\"token\":\"3f9c00aa\",\"token_lifetime_s\":30,\"wake_from_off\":false,"
        "\"last_refusal\":\"Windows refused: \\\"Access is denied.\\\"\",\"last_refusal_unconfirmed\":true}";
static const char *const ERROR_BODY =
        "{ \"error\": { \"code\": \"host_control_required\",\n"
        "             \"message\": \"Turn on Host control for this device in the Rubylight console.\",\n"
        "             \"setting\": null, \"retry_after_ms\": null } }";

void setUp(void) {
}

void tearDown(void) {
}

void test_reads_nested_values_by_path() {
    size_t len = strlen(POWER_BUSY);
    bool value = true;
    TEST_ASSERT_TRUE(gs_json_bool(POWER_BUSY, len, "restart.available", &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_TRUE(gs_json_bool(POWER_BUSY, len, "wake_from_off", &value));
    TEST_ASSERT_TRUE(value);
    /* Present but not a boolean, nested under another key, or at another level. */
    TEST_ASSERT_FALSE(gs_json_bool(POWER_BUSY, len, "token_lifetime_s", &value));
    TEST_ASSERT_FALSE(gs_json_bool(POWER_BUSY, len, "shutdown", &value));
    TEST_ASSERT_FALSE(gs_json_bool(POWER_BUSY, len, "restart.token", &value));
    TEST_ASSERT_FALSE(gs_json_bool(POWER_BUSY, len, "available", &value));
    char *reason = gs_json_string(POWER_BUSY, len, "shutdown.reason");
    TEST_ASSERT_EQUAL_STRING("Living room TV is streaming from this PC.", reason);
    free(reason);
    TEST_ASSERT_NULL(gs_json_string(POWER_BUSY, len, "token"));
}

void test_reads_the_error_body() {
    size_t len = strlen(ERROR_BODY);
    char *code = gs_json_string(ERROR_BODY, len, "error.code");
    char *message = gs_json_string(ERROR_BODY, len, "error.message");
    TEST_ASSERT_EQUAL_STRING("host_control_required", code);
    TEST_ASSERT_EQUAL_STRING("Turn on Host control for this device in the Rubylight console.", message);
    free(code);
    free(message);
}

void test_unescapes_strings_to_utf8() {
    const char *json = "{\"a\":\"tab\\there \\\\ \\/ caf\\u00e9 \\ud83d\\ude00 \\u20ac\"}";
    char *value = gs_json_string(json, strlen(json), "a");
    TEST_ASSERT_EQUAL_STRING("tab\there \\ / caf\xC3\xA9 \xF0\x9F\x98\x80 \xE2\x82\xAC", value);
    free(value);
}

void test_keys_inside_strings_and_arrays_do_not_match() {
    const char *json = "{\"note\":\"\\\"token\\\":\\\"x\\\"\",\"list\":[{\"token\":\"y\"},2,[3]],\"token\":\"z\"}";
    char *token = gs_json_string(json, strlen(json), "token");
    TEST_ASSERT_EQUAL_STRING("z", token);
    free(token);
}

void test_malformed_documents_find_nothing() {
    bool value;
    const char *cut = "{\"restart\":{\"available\":tru";
    TEST_ASSERT_FALSE(gs_json_bool(cut, strlen(cut), "restart.available", &value));
    const char *unquoted = "{restart:{\"available\":true}}";
    TEST_ASSERT_FALSE(gs_json_bool(unquoted, strlen(unquoted), "restart.available", &value));
    const char *open_string = "{\"a\":\"no end";
    TEST_ASSERT_NULL(gs_json_string(open_string, strlen(open_string), "a"));
    const char *bad_escape = "{\"a\":\"\\q\"}";
    TEST_ASSERT_NULL(gs_json_string(bad_escape, strlen(bad_escape), "a"));
    TEST_ASSERT_FALSE(gs_json_bool("", 0, "a", &value));
    TEST_ASSERT_NULL(gs_json_string(NULL, 0, "a"));
}

void test_parses_a_busy_pc() {
    GS_HOST_POWER power;
    TEST_ASSERT_TRUE(gs_host_power_parse(POWER_BUSY, strlen(POWER_BUSY), &power));
    TEST_ASSERT_FALSE(power.restart.available);
    TEST_ASSERT_FALSE(power.shutdown.available);
    TEST_ASSERT_EQUAL_STRING("Living room TV is streaming from this PC.", power.restart.reason);
    TEST_ASSERT_NULL(power.token);
    TEST_ASSERT_TRUE(power.wake_from_off);
    TEST_ASSERT_NULL(power.last_refusal);
    gs_host_power_clear(&power);
    TEST_ASSERT_NULL(power.restart.reason);
}

void test_parses_an_idle_pc_and_its_last_refusal() {
    GS_HOST_POWER power;
    TEST_ASSERT_TRUE(gs_host_power_parse(POWER_IDLE, strlen(POWER_IDLE), &power));
    TEST_ASSERT_TRUE(power.restart.available);
    TEST_ASSERT_NULL(power.restart.reason);
    TEST_ASSERT_EQUAL_STRING("3f9c00aa", power.token);
    TEST_ASSERT_FALSE(power.wake_from_off);
    TEST_ASSERT_EQUAL_STRING("Windows refused: \"Access is denied.\"", power.last_refusal);
    TEST_ASSERT_EQUAL_PTR(&power.shutdown, gs_host_power_get_action(&power, GS_POWER_SHUTDOWN));
    gs_host_power_clear(&power);
}

void test_a_body_without_power_state_is_refused() {
    GS_HOST_POWER power;
    TEST_ASSERT_FALSE(gs_host_power_parse(ERROR_BODY, strlen(ERROR_BODY), &power));
    TEST_ASSERT_NULL(power.token);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_reads_nested_values_by_path);
    RUN_TEST(test_reads_the_error_body);
    RUN_TEST(test_unescapes_strings_to_utf8);
    RUN_TEST(test_keys_inside_strings_and_arrays_do_not_match);
    RUN_TEST(test_malformed_documents_find_nothing);
    RUN_TEST(test_parses_a_busy_pc);
    RUN_TEST(test_parses_an_idle_pc_and_its_last_refusal);
    RUN_TEST(test_a_body_without_power_state_is_refused);
    return UNITY_END();
}
