if(NOT DEFINED TRANSPORT)
    set(TRANSPORT "BROADCAST")
endif()

if(NOT WCAN_COMPONENT_NAME)
    if(TRANSPORT STREQUAL "BROADCAST")
        set(WCAN_COMPONENT_NAME "wcan_broadcast")
    elseif(TRANSPORT STREQUAL "MULTICAST")
        set(WCAN_COMPONENT_NAME "wcan_multicast")
    elseif(TRANSPORT STREQUAL "UNICAST")
        set(WCAN_COMPONENT_NAME "wcan_broadcast")
    endif()
endif()

message(STATUS "Main component using WCAN variant: ${WCAN_COMPONENT_NAME}")

idf_component_register(SRCS "main.cpp" "runtime_config.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES
                           ${WCAN_COMPONENT_NAME}
                       PRIV_REQUIRES
                           wcan
                           driver
                           esp_event
                           esp_hw_support
                           esp_netif
                           esp_rom
                           esp_timer
                           esp_wifi
                           esp_driver_usb_serial_jtag
                           nvs_flash)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WCAN_TRANSPORT_NAME=\"${TRANSPORT}\")
message(STATUS "Building runtime-configurable main for TRANSPORT=${TRANSPORT}")
