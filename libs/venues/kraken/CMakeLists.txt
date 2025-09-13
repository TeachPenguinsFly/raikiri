add_library(raikiri_venue_kraken STATIC
  src/kraken_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/book.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/sign.cpp
  src/ws.cpp
  src/catalogue.cpp
  src/envelope.cpp
  src/market_data.cpp
  src/orders.cpp
  src/positions.cpp
)
add_library(raikiri::venue_kraken ALIAS raikiri_venue_kraken)

target_include_directories(raikiri_venue_kraken PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# OpenSSL is PRIVATE and confined to src/sign.cpp: Kraken's Authent chain is
# SHA-256 then HMAC-SHA-512 over raw bytes with a base64-decoded key, and
# libs/net exposes none of those three steps.
target_link_libraries(raikiri_venue_kraken
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::config simdjson::simdjson raikiri::warnings)
