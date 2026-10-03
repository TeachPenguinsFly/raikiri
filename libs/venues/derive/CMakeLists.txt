add_library(raikiri_venue_derive STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/map.cpp
  src/types/wire.cpp
  src/marketdata/client.cpp
)
add_library(raikiri::venue_derive ALIAS raikiri_venue_derive)

target_include_directories(raikiri_venue_derive PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_derive
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::options raikiri::wire
  PRIVATE raikiri::config raikiri::log simdjson::simdjson raikiri::warnings)
