add_library(raikiri_venue_bitfinex STATIC
  src/bitfinex_venue.cpp
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
  src/sign.cpp
  src/ws.cpp
)
add_library(raikiri::venue_bitfinex ALIAS raikiri_venue_bitfinex)

target_include_directories(raikiri_venue_bitfinex PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# OpenSSL is PRIVATE too, and confined to src/sign.cpp: Bitfinex signs with
# HMAC-SHA384, which libs/net carries neither of, and SHA-384 is not SHA-512
# truncated — it has a different initial state, so no caller can synthesise it.
target_link_libraries(raikiri_venue_bitfinex
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::config simdjson::simdjson raikiri::warnings)
