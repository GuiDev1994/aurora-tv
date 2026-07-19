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
    settings.use_ntsc_refresh = true;
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
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 0;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);

    settings.stream.fps = 60;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);

    /* NTSC off → integer presets clear fractional rate */
    settings.use_ntsc_refresh = false;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11988;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);

    settings.stream.fps = 60;
    settings.client_refresh_rate_x100 = 5994;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);

    /* Custom fractional (not auto-NTSC) preserved with NTSC off */
    settings.use_ntsc_refresh = false;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11994;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11994, settings.client_refresh_rate_x100);

    /* Custom preserved with NTSC on */
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11994;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11994, settings.client_refresh_rate_x100);

    /* Preset apply forces NTSC or integer */
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11994;
    settings_apply_ntsc_preset_refresh(&settings, 120);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);

    settings.use_ntsc_refresh = false;
    settings_apply_ntsc_preset_refresh(&settings, 120);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
#endif
}

void testDefaultNtscAndSmoothFlags() {
    TEST_ASSERT_FALSE(settings.use_ntsc_refresh);
#if defined(TARGET_WEBOS)
    TEST_ASSERT_TRUE(settings.smooth_frame_pacing);
    TEST_ASSERT_TRUE(settings.pause_at_decode_time);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
#else
    TEST_ASSERT_FALSE(settings.smooth_frame_pacing);
    TEST_ASSERT_TRUE(settings.pause_at_decode_time);
#endif
}

void testIdrRefreshIntervalMsDefaultAndClamp() {
    TEST_ASSERT_EQUAL_INT(0, settings.idr_refresh_interval_ms);

    settings.idr_refresh_interval_ms = 750;
    char *ini_backup = settings.ini_path;
    settings.ini_path = "settings_idr_tmp.ini";
    TEST_ASSERT_TRUE(settings_save(&settings));

    settings.idr_refresh_interval_ms = 0;
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(1000, settings.idr_refresh_interval_ms); /* snapped from 750→1000 via save path */

    /* Re-save exact 500 and confirm round-trip */
    settings.idr_refresh_interval_ms = 500;
    TEST_ASSERT_TRUE(settings_save(&settings));
    settings.idr_refresh_interval_ms = 0;
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(500, settings.idr_refresh_interval_ms);
    settings.ini_path = ini_backup;
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testReadINI);
    RUN_TEST(testWriteINI);
    RUN_TEST(testSyncRefreshRate);
    RUN_TEST(testReconcileRefreshRate);
    RUN_TEST(testNtscRefreshRateMapping);
    RUN_TEST(testDefaultNtscAndSmoothFlags);
    RUN_TEST(testIdrRefreshIntervalMsDefaultAndClamp);
    return UNITY_END();
}
