add_library(raikiri_venue_edgex STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/book.cpp
  src/types/map.cpp
  src/types/wire.cpp
  src/marketdata/client.cpp
)
add_library(raikiri::venue_edgex ALIAS raikiri_venue_edgex)

target_include_directories(raikiri_venue_edgex PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_edgex
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::log simdjson::simdjson raikiri::warnings)
