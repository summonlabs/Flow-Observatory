# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.

add_executable(foctl main.cpp)
target_link_libraries(foctl PRIVATE flowobs $<BUILD_INTERFACE:flowobs_warnings>)
target_compile_features(foctl PRIVATE cxx_std_20)
set_target_properties(foctl PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

install(TARGETS foctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
