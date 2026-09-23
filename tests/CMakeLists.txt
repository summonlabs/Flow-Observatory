# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#
# No test declares a timeout. A test either completes or the process does not
# terminate; nothing is ever reported as passing because a deadline expired.

add_library(fotest STATIC fotest.cpp)
target_include_directories(fotest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fotest PUBLIC flowobs $<BUILD_INTERFACE:flowobs_warnings>)
set_target_properties(fotest PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

function(fo_add_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE fotest)
  add_test(NAME ${target} COMMAND ${target} --seed=${FLOWOBS_TEST_SEED})
endfunction()

set(FLOWOBS_TEST_SEED "20260101" CACHE STRING "Seed for the randomised test cases")

fo_add_test(fo_test_unit
  unit_basics.cpp
  unit_reconcile.cpp
  unit_persistence.cpp
)

fo_add_test(fo_test_engine
  unit_engine.cpp
  integration_lifecycle.cpp
)

fo_add_test(fo_test_property
  property_determinism.cpp
  property_invariants.cpp
)

fo_add_test(fo_test_adversarial
  adversarial_evidence.cpp
  adversarial_persistence.cpp
)
# The collision-handling test drives the identity registry directly, because a
# 64-bit hash collision cannot be constructed on demand.
target_include_directories(fo_test_adversarial PRIVATE ${CMAKE_SOURCE_DIR}/src)

fo_add_test(fo_test_concurrency
  concurrency_engine.cpp
  concurrency_lockorder.cpp
)
# The lock-order audit test deliberately reaches into the internal guard header:
# it is a white-box test of the audit mechanism itself.
target_include_directories(fo_test_concurrency PRIVATE ${CMAKE_SOURCE_DIR}/src)

fo_add_test(fo_test_restart
  restart_recovery.cpp
)

fo_add_test(fo_test_fuzz
  fuzz_parsers.cpp
)

if(FLOWOBS_BUILD_TOOLS)
  fo_add_test(fo_test_transport
    transport_process.cpp
  )
  # The transport test drives foctl as an independent operating-system process.
  set_tests_properties(fo_test_transport PROPERTIES
    ENVIRONMENT "FLOWOBS_FOCTL=$<TARGET_FILE:foctl>")
endif()

# Independent downstream consumer proof: installs the package into a scratch
# prefix, configures a separate CMake project with find_package, builds it and
# runs it as a real external process.
add_test(NAME fo_test_downstream
  COMMAND ${CMAKE_COMMAND}
    -DFLOWOBS_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DFLOWOBS_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DFLOWOBS_GENERATOR=${CMAKE_GENERATOR}
    -DFLOWOBS_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DFLOWOBS_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DFLOWOBS_RC_COMPILER=${CMAKE_RC_COMPILER}
    -DFLOWOBS_MT=${CMAKE_MT}
    -DFLOWOBS_SANITIZE=${FLOWOBS_ENABLE_ASAN}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/downstream/run_downstream_proof.cmake)
