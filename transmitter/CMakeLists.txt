# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Select WCAN transport variant. Two parallel components implement different
# methodologies; only one is compiled and linked per build. The unselected
# component is excluded so its source files never enter the build.
# Usage: idf.py -B build_dir -DTRANSPORT=BROADCAST build
#    or: idf.py -B build_dir -DTRANSPORT=MULTICAST build
if(NOT DEFINED TRANSPORT)
    set(TRANSPORT "BROADCAST")
endif()

if(TRANSPORT STREQUAL "BROADCAST")
    set(WCAN_COMPONENT_NAME "wcan_broadcast" CACHE STRING "Selected WCAN component")
    list(APPEND EXCLUDE_COMPONENTS wcan_multicast)
elseif(TRANSPORT STREQUAL "MULTICAST")
    set(WCAN_COMPONENT_NAME "wcan_multicast" CACHE STRING "Selected WCAN component")
    list(APPEND EXCLUDE_COMPONENTS wcan_broadcast)
else()
    message(FATAL_ERROR "TRANSPORT must be BROADCAST or MULTICAST; got: ${TRANSPORT}")
endif()

message(STATUS "Building with TRANSPORT=${TRANSPORT}, component=${WCAN_COMPONENT_NAME}")

# Optional measurement instrumentation. Enable with -DMEASURE=1 to compile in
# airtime counters, latency logs, cold-start markers, etc. Production builds
# leave it off so the binary is unaffected.
if(DEFINED MEASURE AND MEASURE)
    add_compile_definitions(MEASURE_INSTR)
    message(STATUS "MEASURE instrumentation: ENABLED")
else()
    message(STATUS "MEASURE instrumentation: disabled")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensor_acquirer)
