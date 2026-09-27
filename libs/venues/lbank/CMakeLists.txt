add_library(raikiri_venue_lbank STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/map.cpp
  src/types/wire.cpp
  src/marketdata/perp.cpp
  src/marketdata/spot.cpp
)
add_library(raikiri::venue_lbank ALIAS raikiri_venue_lbank)

target_include_directories(raikiri_venue_lbank PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_lbank
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::log simdjson::simdjson raikiri::warnings)
