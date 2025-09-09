add_library(raikiri_venue_coinbase STATIC
  src/coinbase_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/book.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/sign.cpp
  src/ws.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_coinbase ALIAS raikiri_venue_coinbase)

target_include_directories(raikiri_venue_coinbase PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_coinbase
  PUBLIC raikiri::domain raikiri::book raikiri::wire raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson raikiri::config raikiri::warnings)
