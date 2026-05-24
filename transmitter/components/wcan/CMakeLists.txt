idf_component_register(SRCS "Packet.cpp" "Stats.cpp" "TransceiverBase.cpp"
                       INCLUDE_DIRS "inc"
                       REQUIRES esp_wifi esp_timer
                       PRIV_REQUIRES driver)

