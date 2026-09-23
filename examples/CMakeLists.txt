# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.

add_executable(fo_example_embedded embedded_observe.cpp)
target_link_libraries(fo_example_embedded PRIVATE flowobs $<BUILD_INTERFACE:flowobs_warnings>)
target_compile_features(fo_example_embedded PRIVATE cxx_std_20)

add_executable(fo_example_multi_source multi_source_reconciliation.cpp)
target_link_libraries(fo_example_multi_source PRIVATE flowobs $<BUILD_INTERFACE:flowobs_warnings>)
target_compile_features(fo_example_multi_source PRIVATE cxx_std_20)

add_executable(fo_example_restart restart_and_recovery.cpp)
target_link_libraries(fo_example_restart PRIVATE flowobs $<BUILD_INTERFACE:flowobs_warnings>)
target_compile_features(fo_example_restart PRIVATE cxx_std_20)
