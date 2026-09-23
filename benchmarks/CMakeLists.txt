# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.

add_executable(fo_bench fo_bench.cpp)
target_link_libraries(fo_bench PRIVATE flowobs $<BUILD_INTERFACE:flowobs_warnings>)
target_compile_features(fo_bench PRIVATE cxx_std_20)
set_target_properties(fo_bench PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

if(BUILD_SHARED_LIBS)
  # Benchmarks must run from a build tree without an install step.
  add_custom_command(TARGET fo_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:flowobs> $<TARGET_FILE_DIR:fo_bench>)
endif()
