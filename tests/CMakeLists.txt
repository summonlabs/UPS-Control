# Test suite.
#
# No test is given a timeout. CTest timeouts, timeout wrappers, and watchdog
# processes are prohibited in this repository: a test that does not finish is a
# defect to diagnose, not to bound.

# The probe is a real program that the multiprocess and crash tests start as an
# independent operating-system process.
add_executable(uc_probe support/uc_probe.cpp)
target_link_libraries(uc_probe PRIVATE ups_control)
ups_control_apply_strict_options(uc_probe)

add_library(uc_test_support STATIC
  support/test_harness.cpp
  support/fixture.cpp
  support/proc.cpp)
target_include_directories(uc_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(uc_test_support PUBLIC ups_control)
target_compile_definitions(uc_test_support PUBLIC
  "UC_PROBE_EXECUTABLE=\"$<TARGET_FILE:uc_probe>\"")
add_dependencies(uc_test_support uc_probe)
ups_control_apply_strict_options(uc_test_support)

if(TARGET ups_control_cli)
  target_compile_definitions(uc_test_support PUBLIC
    "UC_CLI_EXECUTABLE=\"$<TARGET_FILE:ups_control_cli>\"")
  add_dependencies(uc_test_support ups_control_cli)
endif()

function(ups_control_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE uc_test_support)
  # Adversarial artifacts are produced with the same encoder and checksum the
  # library uses, so the tests need the internal headers.
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  ups_control_apply_strict_options(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

ups_control_add_test(uc_test_types test_types.cpp)
ups_control_add_test(uc_test_graph test_graph.cpp)
ups_control_add_test(uc_test_evidence test_evidence.cpp)
ups_control_add_test(uc_test_obligations test_obligations.cpp)
ups_control_add_test(uc_test_authority test_authority.cpp)
ups_control_add_test(uc_test_transitions test_transitions.cpp)
ups_control_add_test(uc_test_attempts test_attempts.cpp)
ups_control_add_test(uc_test_persistence test_persistence.cpp)
ups_control_add_test(uc_test_recovery test_recovery.cpp)
ups_control_add_test(uc_test_crash test_crash.cpp)
ups_control_add_test(uc_test_concurrency test_concurrency.cpp)
ups_control_add_test(uc_test_multiprocess test_multiprocess.cpp)
ups_control_add_test(uc_test_property test_property.cpp)
ups_control_add_test(uc_test_adversarial test_adversarial.cpp)

if(TARGET ups_control_cli)
  ups_control_add_test(uc_test_cli test_cli.cpp)
endif()

# The downstream consumer is configured, built, and run out of tree against an
# installed package. It is enabled by pointing UPS_CONTROL_DOWNSTREAM_PREFIX at an
# install prefix produced by this project.
if(UPS_CONTROL_DOWNSTREAM_PREFIX)
  add_test(NAME uc_test_downstream_install
    COMMAND "${CMAKE_COMMAND}"
      -DUC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DUC_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/downstream
      -DUC_PREFIX=${UPS_CONTROL_DOWNSTREAM_PREFIX}
      -DUC_GENERATOR=${CMAKE_GENERATOR}
      -DUC_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DUC_CONFIG=$<CONFIG>
      -P "${CMAKE_SOURCE_DIR}/cmake/RunDownstreamCheck.cmake")
  set_tests_properties(uc_test_downstream_install PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
