add_library(raikiri_common STATIC
  src/fixed_point.cpp
)
add_library(raikiri::common ALIAS raikiri_common)

target_include_directories(raikiri_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_common PRIVATE raikiri::warnings)
