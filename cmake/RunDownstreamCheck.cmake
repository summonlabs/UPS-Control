# Configures, builds, and runs the out-of-tree downstream consumer against an
# installed UPS Control package. Invoked by CTest when
# UPS_CONTROL_DOWNSTREAM_PREFIX is set, and by the release verification steps.
#
# A nested CMake project needs a usable C++ toolchain in this process's
# environment. On Windows with MSVC that environment is produced by
# vcvars64.bat, so this script locates it through vswhere (never through a
# hard-coded path) and runs the nested commands through it. When no toolchain can
# be reached the check fails with the exact reason rather than reporting a pass it
# did not earn.

foreach(required UC_SOURCE_DIR UC_BINARY_DIR UC_PREFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "RunDownstreamCheck.cmake requires -D${required}=...")
  endif()
endforeach()

set(uc_vcvars "")
if(WIN32 AND DEFINED UC_VCVARS AND NOT UC_VCVARS STREQUAL "")
  set(uc_vcvars "${UC_VCVARS}")
elseif(WIN32)
  set(uc_program_files_x86 "$ENV{ProgramFiles\(x86\)}")
  set(uc_vswhere "${uc_program_files_x86}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(EXISTS "${uc_vswhere}")
    execute_process(COMMAND "${uc_vswhere}" -latest -products * -requires
                            Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                            -property installationPath
                    OUTPUT_VARIABLE uc_vs_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT uc_vs_path STREQUAL "")
      set(uc_vcvars "${uc_vs_path}/VC/Auxiliary/Build/vcvars64.bat")
    endif()
  endif()
endif()

set(uc_run_serial 0)
function(uc_run description)
  math(EXPR uc_run_serial "${uc_run_serial} + 1")
  if(WIN32 AND NOT uc_vcvars STREQUAL "" AND EXISTS "${uc_vcvars}")
    set(uc_batch "${UC_BINARY_DIR}/uc-run-${uc_run_serial}.bat")
    set(uc_script "@echo off\r\ncall \"${uc_vcvars}\" >nul 2>&1\r\n")
    foreach(argument IN LISTS ARGN)
      set(uc_script "${uc_script}\"${argument}\" ")
    endforeach()
    set(uc_script "${uc_script}\r\nexit /b %ERRORLEVEL%\r\n")
    file(WRITE "${uc_batch}" "${uc_script}")
    execute_process(COMMAND cmd /c "${uc_batch}"
      RESULT_VARIABLE uc_result
      OUTPUT_VARIABLE uc_output
      ERROR_VARIABLE uc_output)
  else()
    execute_process(COMMAND ${ARGN}
      RESULT_VARIABLE uc_result
      OUTPUT_VARIABLE uc_output
      ERROR_VARIABLE uc_output)
  endif()

  if(NOT uc_result EQUAL 0)
    if(uc_output MATCHES "No CMAKE_CXX_COMPILER could be found" OR
       uc_output MATCHES "CMAKE_CXX_COMPILER not set")
      message(FATAL_ERROR
              "${description} could not run because no C++ compiler is reachable from this "
              "environment. Run the suite from a developer environment (for MSVC, after "
              "vcvars64.bat) to execute the installed-package check for real.\n${uc_output}")
    endif()
    if(uc_output MATCHES "Could not find a package configuration file provided by \"UpsControl\"")
      message(FATAL_ERROR
              "${description} failed because no UPS Control package was found under "
              "'${UC_PREFIX}'. Install the project into that prefix first, for example "
              "'cmake --install <build-dir> --prefix ${UC_PREFIX}'.\n${uc_output}")
    endif()
    message(FATAL_ERROR "${description} failed with exit ${uc_result}:\n${uc_output}")
  endif()
  set(uc_last_output "${uc_output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${UC_BINARY_DIR}")
file(MAKE_DIRECTORY "${UC_BINARY_DIR}")

set(configure_command "${CMAKE_COMMAND}"
  -S "${UC_SOURCE_DIR}/downstream/consumer"
  -B "${UC_BINARY_DIR}"
  "-DCMAKE_PREFIX_PATH=${UC_PREFIX}")
if(DEFINED UC_GENERATOR AND NOT UC_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${UC_GENERATOR}")
endif()
if(DEFINED UC_CONFIG AND NOT UC_CONFIG STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${UC_CONFIG}")
endif()

# A multi-configuration generator (Visual Studio, Xcode) ignores CMAKE_BUILD_TYPE
# and defaults to Debug, which would link a Debug consumer against a Release
# library. The configuration is therefore passed explicitly to every nested build
# command whenever the caller named one.
set(uc_config_args "")
if(DEFINED UC_CONFIG AND NOT UC_CONFIG STREQUAL "")
  list(APPEND uc_config_args --config "${UC_CONFIG}")
endif()

uc_run("downstream configure" ${configure_command})
uc_run("downstream build" "${CMAKE_COMMAND}" --build "${UC_BINARY_DIR}" ${uc_config_args})
uc_run("downstream run" "${CMAKE_COMMAND}" --build "${UC_BINARY_DIR}" ${uc_config_args}
       --target run_consumer)

message(STATUS "downstream consumer output:\n${uc_last_output}")
