add_library(raikiri_venue_coinbase STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/sign.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/envelope.cpp
  src/types/errors.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/orders.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_coinbase ALIAS raikiri_venue_coinbase)

target_include_directories(raikiri_venue_coinbase PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_coinbase
  PUBLIC raikiri::domain raikiri::book raikiri::wire raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson raikiri::config raikiri::warnings)
