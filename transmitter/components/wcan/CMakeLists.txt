idf_component_register(SRCS "Packet.cpp" "Stats.cpp" "TransceiverBase.cpp"
                       INCLUDE_DIRS "inc"
                       PRIV_REQUIRES esp_wifi driver esp_timer)
