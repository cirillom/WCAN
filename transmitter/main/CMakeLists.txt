if(DEFINED TRANSPORT)
    message(STATUS "Main component using WCAN transport: ${TRANSPORT}")
else()
    message(STATUS "Main component defers WCAN transport selection to project CMake")
endif()

idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES
                           wcan
                           wcan_sensor
                           wcan_test
                       PRIV_REQUIRES
                           esp_event
                           esp_netif
                           esp_wifi
                           nvs_flash)

if(DEFINED TRANSPORT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WCAN_TRANSPORT_NAME=\"${TRANSPORT}\")
endif()
