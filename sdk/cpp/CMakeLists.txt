cmake_minimum_required(VERSION 3.28)
project(raikiri_sdk VERSION 0.1.0 LANGUAGES CXX)

set(RAIKIRI_SDK_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RAIKIRI_SDK_TOP_LEVEL ON)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(RAIKIRI_SDK_TESTS "Build the sdk's tests" ${RAIKIRI_SDK_TOP_LEVEL})

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED CONFIG)
find_package(glaze REQUIRED CONFIG)

add_library(raikiri_sdk STATIC
  src/client.cpp
  src/decimal.cpp
  src/signing.cpp
  src/stream.cpp
  src/transport.cpp
)
add_library(raikiri::sdk ALIAS raikiri_sdk)
set_target_properties(raikiri_sdk PROPERTIES EXPORT_NAME sdk)
target_compile_features(raikiri_sdk PUBLIC cxx_std_23)
target_include_directories(raikiri_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
# nothing of these reaches the public headers; the two header-only ones need not be found again by a user
target_link_libraries(raikiri_sdk PRIVATE $<BUILD_LOCAL_INTERFACE:Boost::headers> $<BUILD_LOCAL_INTERFACE:glaze::glaze>
  OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(TARGET raikiri::warnings)
  target_link_libraries(raikiri_sdk PRIVATE $<BUILD_LOCAL_INTERFACE:raikiri::warnings>)
endif()

if(RAIKIRI_SDK_TESTS)
  find_package(GTest REQUIRED CONFIG)
  enable_testing()
  include(GoogleTest)
  add_executable(raikiri_sdk_test tests/sdk_test.cpp tests/public_headers_test.cpp)
  target_link_libraries(raikiri_sdk_test PRIVATE raikiri::sdk Boost::headers glaze::glaze GTest::gtest_main)
  gtest_discover_tests(raikiri_sdk_test DISCOVERY_TIMEOUT 30)
endif()

if(RAIKIRI_SDK_TOP_LEVEL)
  include(CMakePackageConfigHelpers)
  include(GNUInstallDirs)
  install(TARGETS raikiri_sdk EXPORT raikiri-sdk-targets)
  install(DIRECTORY include/ DESTINATION include)
  install(EXPORT raikiri-sdk-targets NAMESPACE raikiri:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/raikiri-sdk)
  configure_package_config_file(cmake/raikiri-sdk-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/raikiri-sdk-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/raikiri-sdk)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/raikiri-sdk-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/raikiri-sdk)
endif()
