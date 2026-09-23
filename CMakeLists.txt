cmake_minimum_required(VERSION 3.21)

project(FlowObservatory
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral per-flow lifecycle and resource-consumption observation runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Flow-Observatory"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FLOWOBS_IS_TOP_LEVEL ON)
else()
  set(FLOWOBS_IS_TOP_LEVEL OFF)
endif()

option(FLOWOBS_BUILD_TESTS "Build the Flow Observatory test suite" ${FLOWOBS_IS_TOP_LEVEL})
option(FLOWOBS_BUILD_TOOLS "Build the foctl inspection tool" ${FLOWOBS_IS_TOP_LEVEL})
option(FLOWOBS_BUILD_EXAMPLES "Build the examples" ${FLOWOBS_IS_TOP_LEVEL})
option(FLOWOBS_BUILD_BENCHMARKS "Build the benchmarks" ${FLOWOBS_IS_TOP_LEVEL})
option(FLOWOBS_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(FLOWOBS_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(FLOWOBS_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer where supported" OFF)
option(FLOWOBS_INSTALL "Generate install and package rules" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration - first-party warning count must be zero.
# ---------------------------------------------------------------------------
add_library(flowobs_warnings INTERFACE)
if(MSVC)
  target_compile_options(flowobs_warnings INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:ternary /utf-8 /EHsc /MP
    /wd4127 # conditional expression is constant - required by generic templates
  )
  if(FLOWOBS_WARNINGS_AS_ERRORS)
    target_compile_options(flowobs_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(flowobs_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
  )
else()
  target_compile_options(flowobs_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough -Werror=return-type)
  if(FLOWOBS_WARNINGS_AS_ERRORS)
    target_compile_options(flowobs_warnings INTERFACE -Werror)
  endif()
endif()

if(FLOWOBS_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
endif()
if(FLOWOBS_ENABLE_UBSAN AND NOT MSVC)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(FLOWOBS_PUBLIC_HEADERS
  include/flowobs/attribution.hpp
  include/flowobs/checked.hpp
  include/flowobs/checksum.hpp
  include/flowobs/clock.hpp
  include/flowobs/counters.hpp
  include/flowobs/engine.hpp
  include/flowobs/error.hpp
  include/flowobs/export.hpp
  include/flowobs/export_json.hpp
  include/flowobs/flow.hpp
  include/flowobs/flowobs.hpp
  include/flowobs/history.hpp
  include/flowobs/identity.hpp
  include/flowobs/limits.hpp
  include/flowobs/observation.hpp
  include/flowobs/persistence.hpp
  include/flowobs/policy.hpp
  include/flowobs/reconcile.hpp
  include/flowobs/semantics.hpp
  include/flowobs/stats.hpp
  include/flowobs/textproto.hpp
  include/flowobs/time.hpp
  include/flowobs/transport.hpp
  include/flowobs/version.hpp
)

add_library(flowobs
  src/attribution.cpp
  src/checksum.cpp
  src/clock.cpp
  src/counters.cpp
  src/engine.cpp
  src/engine_api.cpp
  src/error.cpp
  src/explain.cpp
  src/export_json.cpp
  src/flow.cpp
  src/history.cpp
  src/identity.cpp
  src/limits.cpp
  src/lockorder.cpp
  src/observation.cpp
  src/obs_codec.cpp
  src/persistence.cpp
  src/policy.cpp
  src/query.cpp
  src/reconcile.cpp
  src/semantics.cpp
  src/stats.cpp
  src/textproto.cpp
  src/time.cpp
  src/transport.cpp
  src/version.cpp
  ${FLOWOBS_PUBLIC_HEADERS}
)
add_library(FlowObservatory::flowobs ALIAS flowobs)

target_include_directories(flowobs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(flowobs PRIVATE $<BUILD_INTERFACE:flowobs_warnings>)
target_compile_features(flowobs PUBLIC cxx_std_20)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(flowobs
    PUBLIC FLOWOBS_SHARED
    PRIVATE FLOWOBS_BUILDING_LIBRARY)
endif()

if(WIN32)
  target_link_libraries(flowobs PRIVATE ws2_32)
endif()

find_package(Threads REQUIRED)
target_link_libraries(flowobs PUBLIC Threads::Threads)

set_target_properties(flowobs PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET default)

# Export a macro-free include variable so downstream vendoring is trivial.
target_compile_definitions(flowobs PUBLIC
  FLOWOBS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  FLOWOBS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  FLOWOBS_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(FLOWOBS_BUILD_TOOLS)
  add_subdirectory(tools/foctl)
endif()
if(FLOWOBS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(FLOWOBS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(FLOWOBS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------
if(FLOWOBS_INSTALL)
  install(TARGETS flowobs
    EXPORT FlowObservatoryTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT FlowObservatoryTargets
    FILE FlowObservatoryTargets.cmake
    NAMESPACE FlowObservatory::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FlowObservatory)

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FlowObservatoryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FlowObservatoryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FlowObservatory)

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/FlowObservatoryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/FlowObservatoryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FlowObservatoryConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FlowObservatory)

  install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/FlowObservatory)
endif()
