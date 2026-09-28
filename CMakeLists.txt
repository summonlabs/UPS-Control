cmake_minimum_required(VERSION 3.21)

project(UpsControl
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral UPS control and lifecycle model for the Data Center Control Plane"
  HOMEPAGE_URL "https://github.com/summonlabs/UPS-Control"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(UPS_CONTROL_IS_TOP_LEVEL ON)
else()
  set(UPS_CONTROL_IS_TOP_LEVEL OFF)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(UPS_CONTROL_IS_TOP_LEVEL)
  option(UPS_CONTROL_BUILD_TESTS "Build the UPS Control test suite" ON)
  option(UPS_CONTROL_BUILD_EXAMPLES "Build the UPS Control examples" ON)
  option(UPS_CONTROL_BUILD_BENCHMARKS "Build the UPS Control benchmarks" ON)
  option(UPS_CONTROL_BUILD_TOOLS "Build the UPS Control command line tool" ON)
else()
  option(UPS_CONTROL_BUILD_TESTS "Build the UPS Control test suite" OFF)
  option(UPS_CONTROL_BUILD_EXAMPLES "Build the UPS Control examples" OFF)
  option(UPS_CONTROL_BUILD_BENCHMARKS "Build the UPS Control benchmarks" OFF)
  option(UPS_CONTROL_BUILD_TOOLS "Build the UPS Control command line tool" OFF)
endif()

option(UPS_CONTROL_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

set(UPS_CONTROL_DOWNSTREAM_PREFIX "" CACHE PATH
  "Install prefix of a previous UPS Control installation; enables the downstream consumer test")

# ---------------------------------------------------------------------------
# First-party compile options
# ---------------------------------------------------------------------------
function(ups_control_apply_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(UPS_CONTROL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wnon-virtual-dtor
      -Wcast-qual
      -Wdouble-promotion)
    if(UPS_CONTROL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
set(UPS_CONTROL_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/ups_control/version.hpp.in"
  "${UPS_CONTROL_GENERATED_INCLUDE_DIR}/ups_control/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(ups_control
  src/status.cpp
  src/ids.cpp
  src/units.cpp
  src/operating.cpp
  src/telemetry.cpp
  src/battery.cpp
  src/refusal.cpp
  src/obligation.cpp
  src/authority.cpp
  src/command.cpp
  src/ups.cpp
  src/evaluation.cpp
  src/state.cpp
  src/store.cpp
  src/adapter.cpp
  src/engine.cpp
  src/detail/codec.cpp
  src/detail/crc32c.cpp
  src/detail/digest.cpp
  src/detail/file_lock.cpp
  src/detail/platform_io.cpp
  src/detail/serialization.cpp)
add_library(UpsControl::ups_control ALIAS ups_control)

target_include_directories(ups_control
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${UPS_CONTROL_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(ups_control PUBLIC cxx_std_20)
ups_control_apply_strict_options(ups_control)

set_target_properties(ups_control PROPERTIES
  OUTPUT_NAME "ups_control"
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

if(BUILD_SHARED_LIBS AND WIN32)
  set_target_properties(ups_control PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
# CLI tool
# ---------------------------------------------------------------------------
if(UPS_CONTROL_BUILD_TOOLS)
  add_executable(ups_control_cli tools/ups_cli.cpp tools/cli_json.cpp)
  set_target_properties(ups_control_cli PROPERTIES OUTPUT_NAME "ups-control")
  target_link_libraries(ups_control_cli PRIVATE ups_control)
  target_include_directories(ups_control_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools")
  ups_control_apply_strict_options(ups_control_cli)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS ups_control
  EXPORT UpsControlTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(UPS_CONTROL_BUILD_TOOLS)
  install(TARGETS ups_control_cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/ups_control"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
install(FILES "${UPS_CONTROL_GENERATED_INCLUDE_DIR}/ups_control/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ups_control")

set(UPS_CONTROL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/UpsControl")

install(EXPORT UpsControlTargets
  NAMESPACE UpsControl::
  FILE UpsControlTargets.cmake
  DESTINATION "${UPS_CONTROL_INSTALL_CMAKEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/UpsControlConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/UpsControlConfig.cmake"
  INSTALL_DESTINATION "${UPS_CONTROL_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/UpsControlConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/UpsControlConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/UpsControlConfigVersion.cmake"
  DESTINATION "${UPS_CONTROL_INSTALL_CMAKEDIR}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/UpsControl")

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
if(UPS_CONTROL_IS_TOP_LEVEL AND (UPS_CONTROL_BUILD_TESTS OR UPS_CONTROL_BUILD_EXAMPLES))
  enable_testing()
endif()

if(UPS_CONTROL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(UPS_CONTROL_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(UPS_CONTROL_BUILD_TESTS)
  add_subdirectory(tests)
endif()
