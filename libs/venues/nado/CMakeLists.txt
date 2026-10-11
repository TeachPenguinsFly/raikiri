add_library(raikiri_venue_nado STATIC
  src/venue.cpp
  src/types/map.cpp
  src/types/wire.cpp
)
add_library(raikiri::venue_nado ALIAS raikiri_venue_nado)

target_include_directories(raikiri_venue_nado PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_nado
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::log simdjson::simdjson raikiri::warnings)
