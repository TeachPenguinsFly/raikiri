add_library(raikiri_venue_kraken STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/envelope.cpp
  src/types/errors.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/orders.cpp
  src/types/positions.cpp
  src/types/sign.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_kraken ALIAS raikiri_venue_kraken)

target_include_directories(raikiri_venue_kraken PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# OpenSSL is PRIVATE and confined to src/types/sign.cpp: Kraken's Authent chain
# is SHA-256 then HMAC-SHA-512 over raw bytes with a base64-decoded key, and
# libs/net exposes none of those three steps.
target_link_libraries(raikiri_venue_kraken
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::config simdjson::simdjson raikiri::warnings)
