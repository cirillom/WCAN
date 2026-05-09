idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES "${WCAN_COMPONENT_NAME}")

# Inject ROLE_SENSOR / ROLE_RECEIVER / ROLE_IDLE as a compile definition.
# Usage: idf.py -B build_dir -DROLE=SENSOR build
#    or: idf.py -B build_dir -DROLE=RECEIVER build
#    or: idf.py -B build_dir -DROLE=IDLE build
if(DEFINED ROLE)
    if(ROLE STREQUAL "SENSOR")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ROLE_SENSOR)
        message(STATUS "Building with ROLE_SENSOR")
    elseif(ROLE STREQUAL "RECEIVER")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ROLE_RECEIVER)
        message(STATUS "Building with ROLE_RECEIVER")
    elseif(ROLE STREQUAL "IDLE")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ROLE_IDLE)
        message(STATUS "Building with ROLE_IDLE")
    else()
        message(FATAL_ERROR "ROLE must be SENSOR, RECEIVER, or IDLE; got: ${ROLE}")
    endif()
else()
    message(FATAL_ERROR "ROLE not defined. Use -DROLE=SENSOR or -DROLE=RECEIVER or -DROLE=IDLE")
endif()

# Optional: sensor sample rate in Hz. Defaults to 200 if not specified.
# Usage: idf.py -B build_dir -DROLE=SENSOR -DSENSOR_HZ=400 build
if(NOT DEFINED SENSOR_HZ)
    set(SENSOR_HZ 200)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE SENSOR_HZ=${SENSOR_HZ})
message(STATUS "Building with SENSOR_HZ=${SENSOR_HZ}")
