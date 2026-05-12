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

# Optional sensor test knobs. Defaults keep a plain single-CAN-ID sensor.
# Usage:
#   idf.py -B build_dir -DROLE=SENSOR -DSENSOR_BASE_CAN_ID=0xc0 \
#       -DSENSOR_CAN_ID_COUNT=4 -DSENSOR_HZ=750 -DWCAN_LINGER_MS=100 build
if(NOT DEFINED SENSOR_HZ)
    set(SENSOR_HZ 200)
endif()
if(NOT DEFINED SENSOR_BASE_CAN_ID)
    set(SENSOR_BASE_CAN_ID 0x100)
endif()
if(NOT DEFINED SENSOR_CAN_ID_COUNT)
    set(SENSOR_CAN_ID_COUNT 1)
endif()
if(NOT DEFINED WCAN_LINGER_MS)
    set(WCAN_LINGER_MS 100)
endif()
if(SENSOR_CAN_ID_COUNT LESS 1 OR SENSOR_CAN_ID_COUNT GREATER 16)
    message(FATAL_ERROR "SENSOR_CAN_ID_COUNT must be between 1 and 16; got ${SENSOR_CAN_ID_COUNT}")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE SENSOR_HZ=${SENSOR_HZ})
target_compile_definitions(${COMPONENT_LIB} PRIVATE SENSOR_BASE_CAN_ID=${SENSOR_BASE_CAN_ID})
target_compile_definitions(${COMPONENT_LIB} PRIVATE SENSOR_CAN_ID_COUNT=${SENSOR_CAN_ID_COUNT})
target_compile_definitions(${COMPONENT_LIB} PRIVATE WCAN_LINGER_MS=${WCAN_LINGER_MS})
if(SENSOR_CAN_ID_COUNT GREATER 1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SENSOR_MULTIPLE_CAN_IDS)
endif()
message(STATUS "Building with SENSOR_HZ=${SENSOR_HZ}")
message(STATUS "Building with SENSOR_BASE_CAN_ID=${SENSOR_BASE_CAN_ID}")
message(STATUS "Building with SENSOR_CAN_ID_COUNT=${SENSOR_CAN_ID_COUNT}")
message(STATUS "Building with WCAN_LINGER_MS=${WCAN_LINGER_MS}")

# Optional receiver active-filter allowlist. Omit RECEIVER_FILTER_COUNT for
# legacy accept-all receivers. Set it to 0 for an intentionally empty allowlist.
if(DEFINED RECEIVER_FILTER_COUNT)
    if(RECEIVER_FILTER_COUNT GREATER 16)
        message(FATAL_ERROR "RECEIVER_FILTER_COUNT supports at most 16 CAN IDs; got ${RECEIVER_FILTER_COUNT}")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RECEIVER_FILTER_COUNT=${RECEIVER_FILTER_COUNT})
    if(RECEIVER_FILTER_COUNT GREATER 0)
        math(EXPR RECEIVER_FILTER_LAST "${RECEIVER_FILTER_COUNT} - 1")
        foreach(i RANGE 0 ${RECEIVER_FILTER_LAST})
            if(DEFINED RECEIVER_FILTER_ID_${i})
                target_compile_definitions(${COMPONENT_LIB} PRIVATE RECEIVER_FILTER_ID_${i}=${RECEIVER_FILTER_ID_${i}})
            else()
                message(FATAL_ERROR "Missing RECEIVER_FILTER_ID_${i}")
            endif()
        endforeach()
    endif()
    message(STATUS "Building with receiver filter count=${RECEIVER_FILTER_COUNT}")
endif()
