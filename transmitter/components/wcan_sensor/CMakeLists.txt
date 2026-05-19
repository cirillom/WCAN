idf_component_register(SRCS "RampCanSensor.cpp"
                       INCLUDE_DIRS "inc"
                       REQUIRES wcan
                       PRIV_REQUIRES esp_timer)
