add_library(raikiri_scan STATIC
  src/board.cpp
)
add_library(raikiri::scan ALIAS raikiri_scan)

target_include_directories(raikiri_scan PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_scan
  PUBLIC raikiri::domain
  PRIVATE raikiri::warnings)
