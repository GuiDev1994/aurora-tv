add_subdirectory(e2e)

add_unit_test(test_settings test_settings.c)
add_unit_test(test_link_hint test_link_hint.c)
add_unit_test(test_host_failure test_host_failure.c)

add_subdirectory(backend)
add_subdirectory(ui)