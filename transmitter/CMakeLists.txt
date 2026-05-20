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
#    or: idf.py -B build_dir -DTRANSPORT=UNICAST build
if(NOT DEFINED TRANSPORT)
    set(TRANSPORT "BROADCAST" CACHE STRING "Transport variant")
endif()

if(TRANSPORT STREQUAL "BROADCAST")
    set(WCAN_COMPONENT_NAME "wcan_broadcast" CACHE INTERNAL "" FORCE)
    set(WCAN_EXCLUDED_COMPONENT_NAME "wcan_multicast")
    add_compile_definitions(WCAN_TRANSPORT_BROADCAST)
elseif(TRANSPORT STREQUAL "MULTICAST")
    set(WCAN_COMPONENT_NAME "wcan_multicast" CACHE INTERNAL "" FORCE)
    set(WCAN_EXCLUDED_COMPONENT_NAME "wcan_broadcast")
    add_compile_definitions(WCAN_TRANSPORT_MULTICAST)
elseif(TRANSPORT STREQUAL "UNICAST")
    set(WCAN_COMPONENT_NAME "wcan_broadcast" CACHE INTERNAL "" FORCE) # Uses broadcast implementation for now
    set(WCAN_EXCLUDED_COMPONENT_NAME "wcan_multicast")
    add_compile_definitions(WCAN_TRANSPORT_BROADCAST)
else()
    message(FATAL_ERROR "TRANSPORT must be BROADCAST, MULTICAST or UNICAST; got: ${TRANSPORT}")
endif()

list(APPEND EXCLUDE_COMPONENTS ${WCAN_EXCLUDED_COMPONENT_NAME})
list(REMOVE_DUPLICATES EXCLUDE_COMPONENTS)

if(COMPONENTS)
    list(APPEND COMPONENTS main wcan wcan_sensor wcan_test ${WCAN_COMPONENT_NAME})
    list(REMOVE_ITEM COMPONENTS ${WCAN_EXCLUDED_COMPONENT_NAME})
else()
    set(COMPONENTS main wcan wcan_sensor wcan_test ${WCAN_COMPONENT_NAME})
endif()
list(REMOVE_DUPLICATES COMPONENTS)

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

include(~/esp/esp-idf/tools/cmake/project.cmake)
project(sensor_acquirer)
