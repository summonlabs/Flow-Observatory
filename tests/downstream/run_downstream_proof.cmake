# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#
# Proves the exported CMake package from a genuinely independent consumer:
#   1. install the project into a scratch prefix,
#   2. configure tests/downstream with CMAKE_PREFIX_PATH pointing only there,
#   3. build it,
#   4. run it as a separate process and check its output.
#
# Any failure is fatal, so the CTest entry fails with the exact step.

foreach(required FLOWOBS_SOURCE_DIR FLOWOBS_BINARY_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "downstream proof: ${required} is not set")
  endif()
endforeach()

set(prefix "${FLOWOBS_BINARY_DIR}/downstream-prefix")
set(consumer_build "${FLOWOBS_BINARY_DIR}/downstream-build")

file(REMOVE_RECURSE "${prefix}")
file(REMOVE_RECURSE "${consumer_build}")

set(install_command
  "${CMAKE_COMMAND}" --install "${FLOWOBS_BINARY_DIR}" --prefix "${prefix}"
  --config "${FLOWOBS_BUILD_TYPE}")
execute_process(COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: install failed (${install_result})\n${install_output}\n${install_error}")
endif()

set(config_args
  -S "${FLOWOBS_SOURCE_DIR}/tests/downstream"
  -B "${consumer_build}"
  "-DCMAKE_PREFIX_PATH=${prefix}"
  "-DCMAKE_BUILD_TYPE=${FLOWOBS_BUILD_TYPE}")
if(DEFINED FLOWOBS_GENERATOR AND NOT FLOWOBS_GENERATOR STREQUAL "")
  list(APPEND config_args -G "${FLOWOBS_GENERATOR}")
endif()
if(DEFINED FLOWOBS_CXX_COMPILER AND NOT FLOWOBS_CXX_COMPILER STREQUAL "")
  list(APPEND config_args "-DCMAKE_CXX_COMPILER=${FLOWOBS_CXX_COMPILER}")
endif()
# The consumer is configured in an environment that may not have the toolchain
# on PATH (a test runner started from a plain shell, for instance), so the
# resource and manifest compilers are forwarded explicitly when they are known.
if(DEFINED FLOWOBS_RC_COMPILER AND NOT FLOWOBS_RC_COMPILER STREQUAL "" AND NOT FLOWOBS_RC_COMPILER MATCHES "NOTFOUND")
  list(APPEND config_args "-DCMAKE_RC_COMPILER=${FLOWOBS_RC_COMPILER}")
endif()
if(DEFINED FLOWOBS_MT AND NOT FLOWOBS_MT STREQUAL "" AND NOT FLOWOBS_MT MATCHES "NOTFOUND")
  list(APPEND config_args "-DCMAKE_MT=${FLOWOBS_MT}")
endif()
if(DEFINED FLOWOBS_SANITIZE AND FLOWOBS_SANITIZE)
  list(APPEND config_args "-DFLOWOBS_CONSUMER_ASAN=ON")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} ${config_args}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: configure failed\n${configure_output}\n${configure_error}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} --build "${consumer_build}" --config "${FLOWOBS_BUILD_TYPE}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: build failed\n${build_output}\n${build_error}")
endif()

set(consumer_exe "${consumer_build}/consumer")
if(WIN32)
  set(consumer_exe "${consumer_build}/consumer.exe")
  if(NOT EXISTS "${consumer_exe}")
    # Multi-config generators nest the executable one level deeper.
    file(GLOB_RECURSE found "${consumer_build}/*/consumer.exe")
    if(found)
      list(GET found 0 consumer_exe)
    endif()
  endif()
endif()
if(NOT EXISTS "${consumer_exe}")
  message(FATAL_ERROR "downstream proof: consumer executable was not produced")
endif()

execute_process(COMMAND "${consumer_exe}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream proof: consumer exited with ${run_result}\n${run_output}\n${run_error}")
endif()
string(FIND "${run_output}" "consumer: ok" ok_position)
if(ok_position EQUAL -1)
  message(FATAL_ERROR "downstream proof: consumer did not report success\n${run_output}")
endif()
message(STATUS "downstream proof ok:\n${run_output}")
