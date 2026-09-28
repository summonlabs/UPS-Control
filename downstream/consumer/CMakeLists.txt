cmake_minimum_required(VERSION 3.21)

# Independent out-of-tree consumer of the installed UPS Control package.
# This project is not part of the UPS Control build tree: it is configured
# separately with CMAKE_PREFIX_PATH pointing at an install prefix.

project(UpsControlConsumer
  VERSION 1.0.0
  DESCRIPTION "Out-of-tree consumer of the installed UPS Control package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(UpsControl 1.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE UpsControl::ups_control)

if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /permissive- /utf-8 /WX)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Runs the consumer through a real minimal lifecycle: create a durable store,
# register a unit, record telemetry, evaluate a transition, submit it through a
# synthetic adapter, verify the effect, close, reopen, and audit.
# $<TARGET_FILE:consumer> is used rather than the bare target name so that a
# multi-configuration generator resolves the executable of the configuration that
# is actually being built.
add_custom_target(run_consumer
  COMMAND "$<TARGET_FILE:consumer>" "${CMAKE_CURRENT_BINARY_DIR}/consumer-store/unit.upsstore"
  DEPENDS consumer
  COMMENT "Running the installed-package consumer lifecycle"
  VERBATIM)
