if (TARGET_WEBOS)
    target_sources(moonlight-lib PRIVATE
            ctm_hid.c
            enet_transport.c
            ctm_transport.c
            hid_passthrough_manager.c
            ctm/ctm_state.c
            ctm/ui_devices.c
            ctm/ui_bridge.c
            ctm/controllers/controller_common.c
            ctm/controllers/ds5_acl_tx.c
            ctm/controllers/ds5_hidfd.c
            ctm/controllers/controller_ds4.c
            ctm/controllers/controller_ds5.c
            ctm/controllers/controller_generic.c
            ctm/controllers/controller_steam_puck.c
            ctm/controllers/controller_flydigi.c
            ctm/controllers/controller_xbox.c
            ctm/monitor/ctm_monitor.c)
    target_include_directories(moonlight-lib PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/ctm
            ${CMAKE_CURRENT_SOURCE_DIR}/ctm/controllers
            ${CMAKE_CURRENT_SOURCE_DIR}/ctm/monitor)
    target_link_libraries(moonlight-lib PRIVATE enet pthread)
endif()
