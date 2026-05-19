idf_component_register(SRCS "WcanTest.cpp"
                       INCLUDE_DIRS "inc"
                       REQUIRES wcan wcan_sensor
                       PRIV_REQUIRES driver esp_driver_usb_serial_jtag)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WCAN_TRANSPORT_NAME=\"${TRANSPORT}\")
