set(WCAN_SELECTED_INCLUDE_DIR "../components/${WCAN_COMPONENT_NAME}/inc")

idf_component_register(SRCS "main.cpp" "runtime_config.cpp"
                       INCLUDE_DIRS "." "${WCAN_SELECTED_INCLUDE_DIR}"
                       PRIV_REQUIRES
                           ${WCAN_COMPONENT_NAME}
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
