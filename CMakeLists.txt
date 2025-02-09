cmake_minimum_required(VERSION 3.28)

project(raikiri VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. Use a CMake preset.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(RAIKIRI_BUILD_TESTS "Build unit tests" ON)
option(RAIKIRI_BUILD_BENCH "Build benchmarks" ON)
option(RAIKIRI_WERROR "Treat warnings as errors" ON)
set(RAIKIRI_SANITIZE "" CACHE STRING "Sanitizers, e.g. 'address,undefined' or 'thread'")
set(RAIKIRI_MARCH "" CACHE STRING "Explicit -march target (never 'native'); empty = compiler default")

include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)

if(RAIKIRI_MARCH)
  if(RAIKIRI_MARCH STREQUAL "native")
    message(FATAL_ERROR "-march=native is banned: build host != deploy host causes SIGILL. Name the target explicitly.")
  endif()
  add_compile_options(-march=${RAIKIRI_MARCH})
endif()

find_package(fmt REQUIRED CONFIG)
find_package(quill REQUIRED CONFIG)

if(RAIKIRI_BUILD_TESTS)
  find_package(GTest REQUIRED CONFIG)
  enable_testing()
endif()
if(RAIKIRI_BUILD_BENCH)
  find_package(benchmark REQUIRED CONFIG)
endif()

add_subdirectory(libs/common)
add_subdirectory(libs/log)
add_subdirectory(libs/domain)

if(RAIKIRI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(RAIKIRI_BUILD_BENCH)
  add_subdirectory(bench)
endif()
