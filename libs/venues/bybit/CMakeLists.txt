add_library(raikiri_venue_bybit STATIC
  src/bybit_venue.cpp
  src/book_sync.cpp
  src/data_client.cpp
  src/errors.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_bybit ALIAS raikiri_venue_bybit)

target_include_directories(raikiri_venue_bybit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_bybit
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config simdjson::simdjson raikiri::warnings)
