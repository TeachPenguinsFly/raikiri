add_library(raikiri_venue_hyperliquid STATIC
  src/hyperliquid_venue.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
  src/wire_private.cpp
  src/keccak.cpp
  src/sign.cpp
  src/exchange.cpp
)
add_library(raikiri::venue_hyperliquid ALIAS raikiri_venue_hyperliquid)

target_include_directories(raikiri_venue_hyperliquid PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_hyperliquid
  PUBLIC raikiri::domain raikiri::wire raikiri::exec raikiri::net
# OpenSSL is PRIVATE and appears in no header: it provides the secp256k1 group
# arithmetic behind Wallet, and nothing else. keccak-256 is NOT taken from it —
# OpenSSL ships SHA3-256, which pads differently and is not the same function.
  PRIVATE simdjson::simdjson OpenSSL::Crypto raikiri::config raikiri::warnings)
