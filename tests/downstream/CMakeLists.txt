# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#
# An independent downstream consumer. It uses nothing but find_package and the
# installed headers, and it is configured and built by fo_test_downstream
# against a scratch install prefix, so the exported package is proven from
# outside the build tree.

cmake_minimum_required(VERSION 3.21)
project(FlowObservatoryConsumer LANGUAGES CXX)

find_package(FlowObservatory 1.0 REQUIRED)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE FlowObservatory::flowobs)
target_compile_features(consumer PRIVATE cxx_std_20)

# The consumer has to be instrumented exactly like the library it links, so the
# parent build forwards its sanitizer choice.
option(FLOWOBS_CONSUMER_ASAN "Build the consumer with AddressSanitizer" OFF)
if(FLOWOBS_CONSUMER_ASAN)
  if(MSVC)
    target_compile_options(consumer PRIVATE /fsanitize=address)
    target_link_options(consumer PRIVATE /INCREMENTAL:NO)
  else()
    target_compile_options(consumer PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(consumer PRIVATE -fsanitize=address)
  endif()
endif()
