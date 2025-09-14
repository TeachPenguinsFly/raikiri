add_library(raikiri_venue_bitmex STATIC
  src/bitmex_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/book.cpp
  src/catalogue.cpp
  src/envelope.cpp
  src/errors.cpp
  src/map.cpp
  src/market_data.cpp
  src/orders.cpp
  src/positions.cpp
  src/rest.cpp
  src/ws.cpp
)
add_library(raikiri::venue_bitmex ALIAS raikiri_venue_bitmex)

target_include_directories(raikiri_venue_bitmex PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_bitmex
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config raikiri::portfolio simdjson::simdjson raikiri::warnings)
