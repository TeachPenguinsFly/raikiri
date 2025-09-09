add_library(raikiri_venue_hyperliquid STATIC
  src/hyperliquid_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/errors.cpp
  src/exchange.cpp
  src/keccak.cpp
  src/map.cpp
  src/rest.cpp
  src/sign.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_hyperliquid ALIAS raikiri_venue_hyperliquid)

target_include_directories(raikiri_venue_hyperliquid PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# OpenSSL is PRIVATE and appears in no header: it provides the secp256k1 group
# arithmetic behind Wallet, and nothing else. keccak-256 is NOT taken from it —
# OpenSSL ships SHA3-256, which pads differently and is not the same function.
target_link_libraries(raikiri_venue_hyperliquid
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::config simdjson::simdjson raikiri::warnings)
