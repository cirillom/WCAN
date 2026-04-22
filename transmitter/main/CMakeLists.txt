idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS ".")

# Inject ROLE_SENSOR or ROLE_RECEIVER as a compile definition.
# Usage: idf.py -B build_dir -DROLE=SENSOR build
#    or: idf.py -B build_dir -DROLE=RECEIVER build
if(DEFINED ROLE)
    if(ROLE STREQUAL "SENSOR")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ROLE_SENSOR)
        message(STATUS "Building with ROLE_SENSOR")
    elseif(ROLE STREQUAL "RECEIVER")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ROLE_RECEIVER)
        message(STATUS "Building with ROLE_RECEIVER")
    else()
        message(FATAL_ERROR "ROLE must be SENSOR or RECEIVER, got: ${ROLE}")
    endif()
else()
    message(FATAL_ERROR "ROLE not defined. Use -DROLE=SENSOR or -DROLE=RECEIVER")
endif()
