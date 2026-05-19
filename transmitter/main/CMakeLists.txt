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

idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES
                           ${WCAN_COMPONENT_NAME}
                           wcan_sensor
                           wcan_test
                       PRIV_REQUIRES
                           wcan
                           esp_event
                           esp_netif
                           esp_wifi
                           nvs_flash)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WCAN_TRANSPORT_NAME=\"${TRANSPORT}\")
message(STATUS "Building runtime-configurable main for TRANSPORT=${TRANSPORT}")
