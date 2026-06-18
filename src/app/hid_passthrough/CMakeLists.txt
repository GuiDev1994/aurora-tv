if (TARGET_WEBOS)
    target_sources(moonlight-lib PRIVATE
            ctm_hid.c
            enet_transport.c
            ctm_transport.c
            hid_transport.c
            hid_device.c
            hid_passthrough_manager.c)
    target_include_directories(moonlight-lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(moonlight-lib PRIVATE enet pthread)
endif()
