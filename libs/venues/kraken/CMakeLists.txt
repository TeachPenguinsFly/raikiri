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
  src/types/spot.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/marketdata/spot_client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_kraken ALIAS raikiri_venue_kraken)

target_include_directories(raikiri_venue_kraken PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# openssl is private to src/types/sign.cpp for kraken's sha-256 then hmac-sha-512 chain
target_link_libraries(raikiri_venue_kraken
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::config simdjson::simdjson raikiri::warnings)
