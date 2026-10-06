add_library(raikiri_venue_ostium STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/map.cpp
  src/types/wire.cpp
  src/marketdata/client.cpp
)
add_library(raikiri::venue_ostium ALIAS raikiri_venue_ostium)

target_include_directories(raikiri_venue_ostium PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_ostium
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::log simdjson::simdjson raikiri::warnings)
