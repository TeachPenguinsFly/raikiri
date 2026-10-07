add_library(raikiri_wire STATIC
  src/json.cpp
  src/protobuf.cpp
)
add_library(raikiri::wire ALIAS raikiri_wire)

target_include_directories(raikiri_wire PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_wire
  PUBLIC simdjson::simdjson
  PRIVATE raikiri::warnings)

# the malformed-frame harness, for any project that links raikiri and has gtest
if(TARGET GTest::gtest)
  add_library(raikiri_wire_malformed STATIC malformed/src/harness.cpp)
  add_library(raikiri::wire_malformed ALIAS raikiri_wire_malformed)
  target_include_directories(raikiri_wire_malformed PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/malformed/include>
  )
  target_link_libraries(raikiri_wire_malformed
    PUBLIC GTest::gtest
    PRIVATE raikiri::warnings)
endif()
