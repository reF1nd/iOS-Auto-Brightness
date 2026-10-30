# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.22)
project(ios_auto_brightness VERSION 8.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

file(STRINGS module/module.prop IAB_VERSION_LINE REGEX "^version=")
string(REPLACE "version=" "" IAB_VERSION "${IAB_VERSION_LINE}")

# Contraction would change float results relative to the reference binary;
# fused operations are spelled out with std::fmaf where the reference used fmadd.
set(IAB_WARNINGS -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)

add_library(brightness_core
  core/android_resources.cpp
  core/brightness_map.cpp
  core/config.cpp
  core/dual_sensor.cpp
  core/engine.cpp
  core/nit_controller.cpp
  core/settings_reader.cpp
  core/system_curve.cpp)
target_include_directories(brightness_core PUBLIC core)
target_compile_options(brightness_core PRIVATE ${IAB_WARNINGS})

add_executable(ios_brightness_daemon core/daemon.cpp)
target_link_libraries(ios_brightness_daemon PRIVATE brightness_core ${CMAKE_DL_LIBS})
target_compile_options(ios_brightness_daemon PRIVATE ${IAB_WARNINGS})
target_compile_definitions(ios_brightness_daemon PRIVATE IAB_VERSION="${IAB_VERSION}")
if(ANDROID)
  target_link_options(ios_brightness_daemon PRIVATE -Wl,-z,max-page-size=16384 -s)
endif()

if(BUILD_TESTING)
  foreach(name engine_tests dual_sensor_tests nit_controller_tests brightness_map_tests android_resources_tests settings_tests)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE brightness_core)
    target_compile_options(${name} PRIVATE ${IAB_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
  endforeach()
  add_test(NAME daemon_end_to_end
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/daemon_test.py $<TARGET_FILE:ios_brightness_daemon>)
endif()
