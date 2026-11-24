add_subdirectory(e2e)

add_unit_test(test_settings test_settings.c)
add_unit_test(test_link_hint test_link_hint.c)
add_unit_test(test_adaptive_bitrate test_adaptive_bitrate.c)

add_subdirectory(backend)
add_subdirectory(ui)