#include <stdlib.h>
#include "unity.h"
#include "app_settings.h"
#include "uuidstr.h"

#ifndef FIXTURES_PATH_PREFIX
#define FIXTURES_PATH_PREFIX "./"
#endif

app_settings_t settings;

void setUp() {
    char *dir = malloc(128);
    uuidstr_t uuid;
    uuidstr_random(&uuid);
    snprintf(dir, 128, "/tmp/moonlight-%s", (char *) &uuid);
    settings_initialize(&settings, dir);
}

void tearDown() {
    settings_clear(&settings);
    free(settings.conf_dir);
}

void testReadINI() {
    char *ini_backup = settings.ini_path;
    settings.ini_path = FIXTURES_PATH_PREFIX "settings_read.ini";
    TEST_ASSERT_TRUE(settings_read(&settings));
    settings.ini_path = ini_backup;
}

void testWriteINI() {
    char *ini_backup = settings.ini_path;
    settings.ini_path = "settings_write_tmp.ini";
    TEST_ASSERT_TRUE(settings_save(&settings));
    settings.ini_path = ini_backup;
}

void testSyncRefreshRate() {
#if defined(TARGET_WEBOS)
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11988;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(120, settings.stream.fps);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);
    settings.client_refresh_rate_x100 = 0;
    settings.stream.fps = 60;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(60, settings.stream.fps);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);
#else
    settings.stream.fps = 119;
    settings.client_refresh_rate_x100 = 11988;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(120, settings.stream.fps);
#endif
}

void testNtscRefreshRateMapping() {
    TEST_ASSERT_EQUAL_INT(2997, settings_ntsc_refresh_rate_x100_for_fps(30));
    TEST_ASSERT_EQUAL_INT(5994, settings_ntsc_refresh_rate_x100_for_fps(60));
    TEST_ASSERT_EQUAL_INT(11988, settings_ntsc_refresh_rate_x100_for_fps(120));
    TEST_ASSERT_EQUAL_INT(23976, settings_ntsc_refresh_rate_x100_for_fps(240));
    TEST_ASSERT_EQUAL_INT(0, settings_ntsc_refresh_rate_x100_for_fps(90));
    TEST_ASSERT_EQUAL_INT(0, settings_ntsc_refresh_rate_x100_for_fps(144));
}

void testReconcileRefreshRate() {
    settings.stream.fps = 144;
    settings.client_refresh_rate_x100 = 11988;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
    TEST_ASSERT_EQUAL_INT(144, settings.stream.fps);

#if defined(TARGET_WEBOS)
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 0;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);

    settings.stream.fps = 60;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);
#endif
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testReadINI);
    RUN_TEST(testWriteINI);
    RUN_TEST(testSyncRefreshRate);
    RUN_TEST(testReconcileRefreshRate);
    RUN_TEST(testNtscRefreshRateMapping);
    return UNITY_END();
}