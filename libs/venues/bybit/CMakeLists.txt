add_library(raikiri_venue_bybit STATIC
  src/bybit_venue.cpp
  src/account.cpp
  src/book.cpp
  src/catalogue.cpp
  src/envelope.cpp
  src/market_data.cpp
  src/orders.cpp
  src/positions.cpp
  src/data_client.cpp
  src/errors.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
)
add_library(raikiri::venue_bybit ALIAS raikiri_venue_bybit)

target_include_directories(raikiri_venue_bybit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_bybit
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config simdjson::simdjson raikiri::warnings)
