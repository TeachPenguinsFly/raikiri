add_library(raikiri_venue_hyperliquid STATIC
  src/venue.cpp
  src/exchange.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/keccak.cpp
  src/sign.cpp
  src/types/account.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/envelope.cpp
  src/types/errors.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/orders.cpp
  src/types/positions.cpp
  src/types/post.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_hyperliquid ALIAS raikiri_venue_hyperliquid)

target_include_directories(raikiri_venue_hyperliquid PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# openssl provides only the secp256k1 arithmetic; its sha3-256 pads differently from keccak-256
target_link_libraries(raikiri_venue_hyperliquid
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::config simdjson::simdjson raikiri::warnings)
