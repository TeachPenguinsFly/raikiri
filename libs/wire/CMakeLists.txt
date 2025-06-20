add_library(raikiri_wire STATIC
  src/json.cpp
  src/protobuf.cpp
)
add_library(raikiri::wire ALIAS raikiri_wire)

target_include_directories(raikiri_wire PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_wire PUBLIC simdjson::simdjson PRIVATE raikiri::warnings)
