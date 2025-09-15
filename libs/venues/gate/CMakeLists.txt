add_library(raikiri_venue_gate STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/account.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/envelope.cpp
  src/types/errors.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/orders.cpp
  src/types/positions.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_gate ALIAS raikiri_venue_gate)

target_include_directories(raikiri_venue_gate PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_gate
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config simdjson::simdjson raikiri::warnings)
